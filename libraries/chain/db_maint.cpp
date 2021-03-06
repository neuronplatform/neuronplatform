/*
 * Copyright (c) 2015 Cryptonomex, Inc., and contributors.
 *
 * The MIT License
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <queue>

#include <boost/multiprecision/integer.hpp>

#include <fc/smart_ref_impl.hpp>
#include <fc/uint128.hpp>

#include <graphene/chain/database.hpp>
#include <graphene/chain/fba_accumulator_id.hpp>
#include <graphene/chain/hardfork.hpp>

#include <graphene/chain/account_object.hpp>
#include <graphene/chain/asset_object.hpp>
#include <graphene/chain/budget_record_object.hpp>
#include <graphene/chain/buyback_object.hpp>
#include <graphene/chain/chain_property_object.hpp>
#include <graphene/chain/committee_member_object.hpp>
#include <graphene/chain/fba_object.hpp>
#include <graphene/chain/global_property_object.hpp>
#include <graphene/chain/market_object.hpp>
#include <graphene/chain/special_authority_object.hpp>
#include <graphene/chain/vesting_balance_object.hpp>
#include <graphene/chain/vote_count.hpp>
#include <graphene/chain/witness_object.hpp>
#include <graphene/chain/worker_object.hpp>

namespace graphene { namespace chain {

template<class Index>
vector<std::reference_wrapper<const typename Index::object_type>> database::sort_votable_objects(size_t count) const
{
   using ObjectType = typename Index::object_type;
   const auto& all_objects = get_index_type<Index>().indices();
   count = std::min(count, all_objects.size());
   vector<std::reference_wrapper<const ObjectType>> refs;
   refs.reserve(all_objects.size());
   std::transform(all_objects.begin(), all_objects.end(),
                  std::back_inserter(refs),
                  [](const ObjectType& o) { return std::cref(o); });
   std::partial_sort(refs.begin(), refs.begin() + count, refs.end(),
                   [this](const ObjectType& a, const ObjectType& b)->bool {
      share_type oa_vote = _vote_tally_buffer[a.vote_id];
      share_type ob_vote = _vote_tally_buffer[b.vote_id];
      if( oa_vote != ob_vote )
         return oa_vote > ob_vote;
      return a.vote_id < b.vote_id;
   });

   refs.resize(count, refs.front());
   return refs;
}

template<class... Types>
void database::perform_account_maintenance(std::tuple<Types...> helpers)
{
   const auto& idx = get_index_type<account_index>().indices().get<by_name>();
   for( const account_object& a : idx )
      detail::for_each(helpers, a, detail::gen_seq<sizeof...(Types)>());
}

/// @brief A visitor for @ref worker_type which calls pay_worker on the worker within
struct worker_pay_visitor
{
   private:
      share_type pay;
      database& db;

   public:
      worker_pay_visitor(share_type pay, database& db)
         : pay(pay), db(db) {}

      typedef void result_type;
      template<typename W>
      void operator()(W& worker)const
      {
         worker.pay_worker(pay, db);
      }
};
void database::update_worker_votes()
{
   auto& idx = get_index_type<worker_index>();
   auto itr = idx.indices().get<by_account>().begin();
   bool allow_negative_votes = (head_block_time() < HARDFORK_607_TIME);
   while( itr != idx.indices().get<by_account>().end() )
   {
      modify( *itr, [&]( worker_object& obj ){
         obj.total_votes_for = _vote_tally_buffer[obj.vote_for];
         obj.total_votes_against = allow_negative_votes ? _vote_tally_buffer[obj.vote_against] : 0;
      });
      ++itr;
   }
}

void database::pay_workers( share_type& budget )
{
//   ilog("Processing payroll! Available budget is ${b}", ("b", budget));
   vector<std::reference_wrapper<const worker_object>> active_workers;
   get_index_type<worker_index>().inspect_all_objects([this, &active_workers](const object& o) {
      const worker_object& w = static_cast<const worker_object&>(o);
      auto now = head_block_time();
      if( w.is_active(now) && w.approving_stake() > 0 )
         active_workers.emplace_back(w);
   });

   // worker with more votes is preferred
   // if two workers exactly tie for votes, worker with lower ID is preferred
   std::sort(active_workers.begin(), active_workers.end(), [this](const worker_object& wa, const worker_object& wb) {
      share_type wa_vote = wa.approving_stake();
      share_type wb_vote = wb.approving_stake();
      if( wa_vote != wb_vote )
         return wa_vote > wb_vote;
      return wa.id < wb.id;
   });

   for( uint32_t i = 0; i < active_workers.size() && budget > 0; ++i )
   {
      const worker_object& active_worker = active_workers[i];
      share_type requested_pay = active_worker.daily_pay;
      if( head_block_time() - get_dynamic_global_properties().last_budget_time != fc::days(1) )
      {
         fc::uint128 pay(requested_pay.value);
         pay *= (head_block_time() - get_dynamic_global_properties().last_budget_time).count();
         pay /= fc::days(1).count();
         requested_pay = pay.to_uint64();
      }

      share_type actual_pay = std::min(budget, requested_pay);
      //ilog(" ==> Paying ${a} to worker ${w}", ("w", active_worker.id)("a", actual_pay));
      modify(active_worker, [&](worker_object& w) {
         w.worker.visit(worker_pay_visitor(actual_pay, *this));
      });

      budget -= actual_pay;
   }
}

void database::update_active_witnesses()
{ try {
   assert( _witness_count_histogram_buffer.size() > 0 );
   share_type stake_target = (_total_importance_score-_witness_count_histogram_buffer[0]) / 2;

   /// accounts that vote for 0 or 1 witness do not get to express an opinion on
   /// the number of witnesses to have (they abstain and are non-voting accounts)

   share_type stake_tally = 0; 

   size_t witness_count = 0;
   if( stake_target > 0 )
   {
      while( (witness_count < _witness_count_histogram_buffer.size() - 1)
             && (stake_tally <= stake_target) )
      {
         stake_tally += _witness_count_histogram_buffer[++witness_count];
      }
   }

   const chain_property_object& cpo = get_chain_properties();
   auto wits = sort_votable_objects<witness_index>(std::max(witness_count*2+1, (size_t)cpo.immutable_parameters.min_witness_count));

   const global_property_object& gpo = get_global_properties();

   const auto& all_witnesses = get_index_type<witness_index>().indices();

   for( const witness_object& wit : all_witnesses )
   {
      modify( wit, [&]( witness_object& obj ){
              obj.total_votes = _vote_tally_buffer[wit.vote_id];
              });
   }

   // Update witness authority
   modify( get(GRAPHENE_WITNESS_ACCOUNT), [&]( account_object& a )
   {
      if( head_block_time() < HARDFORK_533_TIME )
      {
         uint64_t total_votes = 0;
         map<account_id_type, uint64_t> weights;
         a.active.weight_threshold = 0;
         a.active.clear();

         for( const witness_object& wit : wits )
         {
            weights.emplace(wit.witness_account, _vote_tally_buffer[wit.vote_id]);
            total_votes += _vote_tally_buffer[wit.vote_id];
         }

         // total_votes is 64 bits. Subtract the number of leading low bits from 64 to get the number of useful bits,
         // then I want to keep the most significant 16 bits of what's left.
         int8_t bits_to_drop = std::max(int(boost::multiprecision::detail::find_msb(total_votes)) - 15, 0);
         for( const auto& weight : weights )
         {
            // Ensure that everyone has at least one vote. Zero weights aren't allowed.
            uint16_t votes = std::max((weight.second >> bits_to_drop), uint64_t(1) );
            a.active.account_auths[weight.first] += votes;
            a.active.weight_threshold += votes;
         }

         a.active.weight_threshold /= 2;
         a.active.weight_threshold += 1;
      }
      else
      {
         vote_counter vc;
         for( const witness_object& wit : wits )
            vc.add( wit.witness_account, _vote_tally_buffer[wit.vote_id] );
         vc.finish( a.active );
      }
   } );

   modify(gpo, [&]( global_property_object& gp ){
      gp.active_witnesses.clear();
      gp.active_witnesses.reserve(wits.size());
      std::transform(wits.begin(), wits.end(),
                     std::inserter(gp.active_witnesses, gp.active_witnesses.end()),
                     [](const witness_object& w) {
         return w.id;
      });
   });

} FC_CAPTURE_AND_RETHROW() }

void database::update_active_committee_members()
{ try {
   assert( _committee_count_histogram_buffer.size() > 0 );
   //share_type stake_target = (_total_importance_score-_witness_count_histogram_buffer[0]) / 2;
   share_type stake_target = (_total_importance_score-_committee_count_histogram_buffer[0]) / 2;

   //TODO: Review this issue
   //share_type stake_target = (_total_voting_stake-_committee_count_histogram_buffer[0]) / 2;

   /// accounts that vote for 0 or 1 witness do not get to express an opinion on
   /// the number of witnesses to have (they abstain and are non-voting accounts)
   uint64_t stake_tally = 0; // _committee_count_histogram_buffer[0];
   size_t committee_member_count = 0;
   if( stake_target > 0 )
      while( (committee_member_count < _committee_count_histogram_buffer.size() - 1)
             && (stake_tally <= stake_target) )
         stake_tally += _committee_count_histogram_buffer[++committee_member_count];
   /*if( stake_target != old_stake_target && old_stake_target > 0 && head_block_time() < fc::time_point_sec(HARDFORK_CORE_353_TIME) )
   {
      uint64_t old_stake_tally = 0;
      size_t old_committee_member_count = 0;
      while( (old_committee_member_count < _committee_count_histogram_buffer.size() - 1)
             && (old_stake_tally <= old_stake_target) )
         old_stake_tally += _committee_count_histogram_buffer[++old_committee_member_count];
      if( old_committee_member_count != committee_member_count
              && (old_committee_member_count > GRAPHENE_DEFAULT_MIN_COMMITTEE_MEMBER_COUNT
                  || committee_member_count > GRAPHENE_DEFAULT_MIN_COMMITTEE_MEMBER_COUNT) )
      {
          ilog( "Committee member count mismatch ${old} / ${new}", ("old",old_committee_member_count)("new", committee_member_count) );
          committee_member_count = old_committee_member_count;
      }
   }*/

   const chain_property_object& cpo = get_chain_properties();
   auto committee_members = sort_votable_objects<committee_member_index>(std::max(committee_member_count*2+1, (size_t)cpo.immutable_parameters.min_committee_member_count));

   for( const committee_member_object& del : committee_members )
   {
      modify( del, [&]( committee_member_object& obj ){
              obj.total_votes = _vote_tally_buffer[del.vote_id];
              });
   }

   // Update committee authorities
   if( !committee_members.empty() )
   {
      modify(get(GRAPHENE_COMMITTEE_ACCOUNT), [&](account_object& a)
      {
         if( head_block_time() < HARDFORK_533_TIME )
         {
            uint64_t total_votes = 0;
            map<account_id_type, uint64_t> weights;
            a.active.weight_threshold = 0;
            a.active.clear();

            for( const committee_member_object& del : committee_members )
            {
               weights.emplace(del.committee_member_account, _vote_tally_buffer[del.vote_id]);
               total_votes += _vote_tally_buffer[del.vote_id];
            }

            // total_votes is 64 bits. Subtract the number of leading low bits from 64 to get the number of useful bits,
            // then I want to keep the most significant 16 bits of what's left.
            int8_t bits_to_drop = std::max(int(boost::multiprecision::detail::find_msb(total_votes)) - 15, 0);
            for( const auto& weight : weights )
            {
               // Ensure that everyone has at least one vote. Zero weights aren't allowed.
               uint16_t votes = std::max((weight.second >> bits_to_drop), uint64_t(1) );
               a.active.account_auths[weight.first] += votes;
               a.active.weight_threshold += votes;
            }

            a.active.weight_threshold /= 2;
            a.active.weight_threshold += 1;
         }
         else
         {
            vote_counter vc;
            for( const committee_member_object& cm : committee_members )
               vc.add( cm.committee_member_account, _vote_tally_buffer[cm.vote_id] );
            vc.finish( a.active );
         }
      } );
      modify(get(GRAPHENE_RELAXED_COMMITTEE_ACCOUNT), [&](account_object& a) {
         a.active = get(GRAPHENE_COMMITTEE_ACCOUNT).active;
      });
   }
   modify(get_global_properties(), [&](global_property_object& gp) {
      gp.active_committee_members.clear();
      std::transform(committee_members.begin(), committee_members.end(),
                     std::inserter(gp.active_committee_members, gp.active_committee_members.begin()),
                     [](const committee_member_object& d) { return d.id; });
   });
} FC_CAPTURE_AND_RETHROW() }

void database::initialize_budget_record( fc::time_point_sec now, budget_record& rec )const
{
   const dynamic_global_property_object& dpo = get_dynamic_global_properties();
   const asset_object& core = asset_id_type(0)(*this);
   const asset_dynamic_data_object& core_dd = core.dynamic_asset_data_id(*this);

   rec.from_initial_reserve = core.reserved(*this);
   rec.from_accumulated_fees = core_dd.accumulated_fees;
   rec.from_unused_witness_budget = dpo.witness_budget;

   if(    (dpo.last_budget_time == fc::time_point_sec())
       || (now <= dpo.last_budget_time) )
   {
      rec.time_since_last_budget = 0;
      return;
   }

   int64_t dt = (now - dpo.last_budget_time).to_seconds();
   rec.time_since_last_budget = uint64_t( dt );

   // We'll consider accumulated_fees to be reserved at the BEGINNING
   // of the maintenance interval.  However, for speed we only
   // call modify() on the asset_dynamic_data_object once at the
   // end of the maintenance interval.  Thus the accumulated_fees
   // are available for the budget at this point, but not included
   // in core.reserved().
   share_type reserve = rec.from_initial_reserve + core_dd.accumulated_fees;
   // Similarly, we consider leftover witness_budget to be burned
   // at the BEGINNING of the maintenance interval.
   reserve += dpo.witness_budget;

   fc::uint128_t budget_u128 = reserve.value;
   budget_u128 *= uint64_t(dt);
   budget_u128 *= GRAPHENE_CORE_ASSET_CYCLE_RATE;
   //round up to the nearest satoshi -- this is necessary to ensure
   //   there isn't an "untouchable" reserve, and we will eventually
   //   be able to use the entire reserve
   budget_u128 += ((uint64_t(1) << GRAPHENE_CORE_ASSET_CYCLE_RATE_BITS) - 1);
   budget_u128 >>= GRAPHENE_CORE_ASSET_CYCLE_RATE_BITS;
   share_type budget;
   if( budget_u128 < reserve.value )
      rec.total_budget = share_type(budget_u128.to_uint64());
   else
      rec.total_budget = reserve;

   return;
}

/**
 * Update the budget for witnesses and workers.
 */
void database::process_budget()
{
   try
   {
      const global_property_object& gpo = get_global_properties();
      const dynamic_global_property_object& dpo = get_dynamic_global_properties();
      const asset_dynamic_data_object& core =
         asset_id_type(0)(*this).dynamic_asset_data_id(*this);
      fc::time_point_sec now = head_block_time();

      int64_t time_to_maint = (dpo.next_maintenance_time - now).to_seconds();
      //
      // The code that generates the next maintenance time should
      //    only produce a result in the future.  If this assert
      //    fails, then the next maintenance time algorithm is buggy.
      //
      assert( time_to_maint > 0 );
      //
      // Code for setting chain parameters should validate
      //    block_interval > 0 (as well as the humans proposing /
      //    voting on changes to block interval).
      //
      assert( gpo.parameters.block_interval > 0 );
      uint64_t blocks_to_maint = (uint64_t(time_to_maint) + gpo.parameters.block_interval - 1) / gpo.parameters.block_interval;

      // blocks_to_maint > 0 because time_to_maint > 0,
      // which means numerator is at least equal to block_interval

      budget_record rec;
      initialize_budget_record( now, rec );
      share_type available_funds = rec.total_budget;

      share_type witness_budget = gpo.parameters.witness_pay_per_block.value * blocks_to_maint;
      rec.requested_witness_budget = witness_budget;
      witness_budget = std::min(witness_budget, available_funds);
      rec.witness_budget = witness_budget;
      available_funds -= witness_budget;

      fc::uint128_t worker_budget_u128 = gpo.parameters.worker_budget_per_day.value;
      worker_budget_u128 *= uint64_t(time_to_maint);
      worker_budget_u128 /= 60*60*24;

      share_type worker_budget;
      if( worker_budget_u128 >= available_funds.value )
         worker_budget = available_funds;
      else
         worker_budget = worker_budget_u128.to_uint64();
      rec.worker_budget = worker_budget;
      available_funds -= worker_budget;

      share_type leftover_worker_funds = worker_budget;
      pay_workers(leftover_worker_funds);
      rec.leftover_worker_funds = leftover_worker_funds;
      available_funds += leftover_worker_funds;

      rec.supply_delta = rec.witness_budget
         + rec.worker_budget
         - rec.leftover_worker_funds
         - rec.from_accumulated_fees
         - rec.from_unused_witness_budget;

      modify(core, [&]( asset_dynamic_data_object& _core )
      {
         _core.current_supply = (_core.current_supply + rec.supply_delta );

         assert( rec.supply_delta ==
                                   witness_budget
                                 + worker_budget
                                 - leftover_worker_funds
                                 - _core.accumulated_fees
                                 - dpo.witness_budget
                                );
         _core.accumulated_fees = 0;
      });

      modify(dpo, [&]( dynamic_global_property_object& _dpo )
      {
         // Since initial witness_budget was rolled into
         // available_funds, we replace it with witness_budget
         // instead of adding it.
         _dpo.witness_budget = witness_budget;
         _dpo.last_budget_time = now;
      });

      create< budget_record_object >( [&]( budget_record_object& _rec )
      {
         _rec.time = head_block_time();
         _rec.record = rec;
      });

      // available_funds is money we could spend, but don't want to.
      // we simply let it evaporate back into the reserve.
   }
   FC_CAPTURE_AND_RETHROW()
}

template< typename Visitor >
void visit_special_authorities( const database& db, Visitor visit )
{
   const auto& sa_idx = db.get_index_type< special_authority_index >().indices().get<by_id>();

   for( const special_authority_object& sao : sa_idx )
   {
      const account_object& acct = sao.account(db);
      if( acct.owner_special_authority.which() != special_authority::tag< no_special_authority >::value )
      {
         visit( acct, true, acct.owner_special_authority );
      }
      if( acct.active_special_authority.which() != special_authority::tag< no_special_authority >::value )
      {
         visit( acct, false, acct.active_special_authority );
      }
   }
}

void update_top_n_authorities( database& db )
{
   visit_special_authorities( db,
   [&]( const account_object& acct, bool is_owner, const special_authority& auth )
   {
      if( auth.which() == special_authority::tag< top_holders_special_authority >::value )
      {
         // use index to grab the top N holders of the asset and vote_counter to obtain the weights

         const top_holders_special_authority& tha = auth.get< top_holders_special_authority >();
         vote_counter vc;
         const auto& bal_idx = db.get_index_type< account_balance_index >().indices().get< by_asset_balance >();
         uint8_t num_needed = tha.num_top_holders;
         if( num_needed == 0 )
            return;

         // find accounts
         const auto range = bal_idx.equal_range( boost::make_tuple( tha.asset ) );
         for( const account_balance_object& bal : boost::make_iterator_range( range.first, range.second ) )
         {
             assert( bal.asset_type == tha.asset );
             if( bal.owner == acct.id )
                continue;
             vc.add( bal.owner, bal.balance.value );
             --num_needed;
             if( num_needed == 0 )
                break;
         }

         db.modify( acct, [&]( account_object& a )
         {
            vc.finish( is_owner ? a.owner : a.active );
            if( !vc.is_empty() )
               a.top_n_control_flags |= (is_owner ? account_object::top_n_control_owner : account_object::top_n_control_active);
         } );
      }
   } );
}

void split_fba_balance(
   database& db,
   uint64_t fba_id,
   uint16_t network_pct,
   uint16_t designated_asset_buyback_pct,
   uint16_t designated_asset_issuer_pct
)
{
   FC_ASSERT( uint32_t(network_pct) + uint32_t(designated_asset_buyback_pct) + uint32_t(designated_asset_issuer_pct) == GRAPHENE_100_PERCENT );
   const fba_accumulator_object& fba = fba_accumulator_id_type( fba_id )(db);
   if( fba.accumulated_fba_fees == 0 )
      return;

   const asset_object& core = asset_id_type(0)(db);
   const asset_dynamic_data_object& core_dd = core.dynamic_asset_data_id(db);

   if( !fba.is_configured(db) )
   {
      ilog( "${n} core given to network at block ${b} due to non-configured FBA", ("n", fba.accumulated_fba_fees)("b", db.head_block_time()) );
      db.modify( core_dd, [&]( asset_dynamic_data_object& _core_dd )
      {
         _core_dd.current_supply -= fba.accumulated_fba_fees;
      } );
      db.modify( fba, [&]( fba_accumulator_object& _fba )
      {
         _fba.accumulated_fba_fees = 0;
      } );
      return;
   }

   fc::uint128_t buyback_amount_128 = fba.accumulated_fba_fees.value;
   buyback_amount_128 *= designated_asset_buyback_pct;
   buyback_amount_128 /= GRAPHENE_100_PERCENT;
   share_type buyback_amount = buyback_amount_128.to_uint64();

   fc::uint128_t issuer_amount_128 = fba.accumulated_fba_fees.value;
   issuer_amount_128 *= designated_asset_issuer_pct;
   issuer_amount_128 /= GRAPHENE_100_PERCENT;
   share_type issuer_amount = issuer_amount_128.to_uint64();

   // this assert should never fail
   FC_ASSERT( buyback_amount + issuer_amount <= fba.accumulated_fba_fees );

   share_type network_amount = fba.accumulated_fba_fees - (buyback_amount + issuer_amount);

   const asset_object& designated_asset = (*fba.designated_asset)(db);

   if( network_amount != 0 )
   {
      db.modify( core_dd, [&]( asset_dynamic_data_object& _core_dd )
      {
         _core_dd.current_supply -= network_amount;
      } );
   }

   fba_distribute_operation vop;
   vop.account_id = *designated_asset.buyback_account;
   vop.fba_id = fba.id;
   vop.amount = buyback_amount;
   if( vop.amount != 0 )
   {
      db.adjust_balance( *designated_asset.buyback_account, asset(buyback_amount) );
      db.push_applied_operation(vop);
   }

   vop.account_id = designated_asset.issuer;
   vop.fba_id = fba.id;
   vop.amount = issuer_amount;
   if( vop.amount != 0 )
   {
      db.adjust_balance( designated_asset.issuer, asset(issuer_amount) );
      db.push_applied_operation(vop);
   }

   db.modify( fba, [&]( fba_accumulator_object& _fba )
   {
      _fba.accumulated_fba_fees = 0;
   } );
}

void distribute_fba_balances( database& db )
{
   split_fba_balance( db, fba_accumulator_id_transfer_to_blind  , 20*GRAPHENE_1_PERCENT, 60*GRAPHENE_1_PERCENT, 20*GRAPHENE_1_PERCENT );
   split_fba_balance( db, fba_accumulator_id_blind_transfer     , 20*GRAPHENE_1_PERCENT, 60*GRAPHENE_1_PERCENT, 20*GRAPHENE_1_PERCENT );
   split_fba_balance( db, fba_accumulator_id_transfer_from_blind, 20*GRAPHENE_1_PERCENT, 60*GRAPHENE_1_PERCENT, 20*GRAPHENE_1_PERCENT );
}

void create_buyback_orders( database& db )
{
   const auto& bbo_idx = db.get_index_type< buyback_index >().indices().get<by_id>();
   const auto& bal_idx = db.get_index_type< account_balance_index >().indices().get< by_account_asset >();

   for( const buyback_object& bbo : bbo_idx )
   {
      const asset_object& asset_to_buy = bbo.asset_to_buy(db);
      assert( asset_to_buy.buyback_account.valid() );

      const account_object& buyback_account = (*(asset_to_buy.buyback_account))(db);
      asset_id_type next_asset = asset_id_type();

      if( !buyback_account.allowed_assets.valid() )
      {
         wlog( "skipping buyback account ${b} at block ${n} because allowed_assets does not exist", ("b", buyback_account)("n", db.head_block_num()) );
         continue;
      }

      while( true )
      {
         auto it = bal_idx.lower_bound( boost::make_tuple( buyback_account.id, next_asset ) );
         if( it == bal_idx.end() )
            break;
         if( it->owner != buyback_account.id )
            break;
         asset_id_type asset_to_sell = it->asset_type;
         share_type amount_to_sell = it->balance;
         next_asset = asset_to_sell + 1;
         if( asset_to_sell == asset_to_buy.id )
            continue;
         if( amount_to_sell == 0 )
            continue;
         if( buyback_account.allowed_assets->find( asset_to_sell ) == buyback_account.allowed_assets->end() )
         {
            wlog( "buyback account ${b} not selling disallowed holdings of asset ${a} at block ${n}", ("b", buyback_account)("a", asset_to_sell)("n", db.head_block_num()) );
            continue;
         }

         try
         {
            transaction_evaluation_state buyback_context(&db);
            buyback_context.skip_fee_schedule_check = true;

            limit_order_create_operation create_vop;
            create_vop.fee = asset( 0, asset_id_type() );
            create_vop.seller = buyback_account.id;
            create_vop.amount_to_sell = asset( amount_to_sell, asset_to_sell );
            create_vop.min_to_receive = asset( 1, asset_to_buy.id );
            create_vop.expiration = time_point_sec::maximum();
            create_vop.fill_or_kill = false;

            limit_order_id_type order_id = db.apply_operation( buyback_context, create_vop ).get< object_id_type >();

            if( db.find( order_id ) != nullptr )
            {
               limit_order_cancel_operation cancel_vop;
               cancel_vop.fee = asset( 0, asset_id_type() );
               cancel_vop.order = order_id;
               cancel_vop.fee_paying_account = buyback_account.id;

               db.apply_operation( buyback_context, cancel_vop );
            }
         }
         catch( const fc::exception& e )
         {
            // we can in fact get here, e.g. if asset issuer of buy/sell asset blacklists/whitelists the buyback account
            wlog( "Skipping buyback processing selling ${as} for ${ab} for buyback account ${b} at block ${n}; exception was ${e}",
                  ("as", asset_to_sell)("ab", asset_to_buy)("b", buyback_account)("n", db.head_block_num())("e", e.to_detail_string()) );
            continue;
         }
      }
   }
   return;
}

void deprecate_annual_members( database& db )
{
   const auto& account_idx = db.get_index_type<account_index>().indices().get<by_id>();
   fc::time_point_sec now = db.head_block_time();
   for( const account_object& acct : account_idx )
   {
      try
      {
         transaction_evaluation_state upgrade_context(&db);
         upgrade_context.skip_fee_schedule_check = true;

         if( acct.is_annual_member( now ) )
         {
            account_upgrade_operation upgrade_vop;
            upgrade_vop.fee = asset( 0, asset_id_type() );
            upgrade_vop.account_to_upgrade = acct.id;
            upgrade_vop.upgrade_to_lifetime_member = true;
            db.apply_operation( upgrade_context, upgrade_vop );
         }
      }
      catch( const fc::exception& e )
      {
         // we can in fact get here, e.g. if asset issuer of buy/sell asset blacklists/whitelists the buyback account
         wlog( "Skipping annual member deprecate processing for account ${a} (${an}) at block ${n}; exception was ${e}",
               ("a", acct.id)("an", acct.name)("n", db.head_block_num())("e", e.to_detail_string()) );
         continue;
      }
   }
   return;
}


using transfer_graph_type2 = std::unordered_multimap<object_id_type, object_id_type>;
using node_type2 = transfer_graph_type2::key_type;

uint32_t rec_cycle_search(const transfer_graph_type2& transfer_graph, object_id_type current, object_id_type cycle_target,
                              uint16_t length, std::unordered_set<object_id_type> visited = std::unordered_set<object_id_type>{})
{
  if (current == cycle_target) return 1;
  if (length == 0) return 0;

  if (visited.find(current) != visited.end()) return 0;
  visited.insert(current);

  size_t bucket_num = transfer_graph.bucket(current);
  auto it = transfer_graph.cbegin(bucket_num);

  uint32_t sum = 0;
  for (; it != transfer_graph.cend(bucket_num); ++it)
    sum += rec_cycle_search(transfer_graph, it->second, cycle_target, length - 1, visited);

  return sum;
}


#if 0
      using transfer_graph_type = std::unordered_multimap<uint64_t, uint64_t>;
      using node_type = transfer_graph_type::key_type;

std::set<node_type> get_gamma(const transfer_graph_type& transfer_graph, const node_type& base_node)
{
      std::set<node_type> gamma{base_node};
      size_t bucket_num = transfer_graph.bucket(base_node);
      auto it = transfer_graph.cbegin(bucket_num);
      for (; it != transfer_graph.cend(bucket_num); ++it)
            gamma.insert(it->second);
      return gamma;
};

fc::optional<std::unordered_set<node_type>> get_core(const transfer_graph_type& transfer_graph, node_type node,
                                                      double epsilon, uint32_t mu)
{
      //size_t bucket_num = transfer_graph.bucket(node);
      //auto it = transfer_graph.cbegin(bucket_num);

      //if (transfer_graph.bucket_size(bucket_num) < mu)
      //      return {};

      std::set<node_type> first = get_gamma(transfer_graph, node);
      if (first.size() < mu)
            return {};

      std::unordered_set<node_type> core;
      auto it = first.cbegin();
      //for (; it != transfer_graph.cend(bucket_num); ++it) {
      for (; it != first.cend(); ++it) {
            std::set<node_type> second = get_gamma(transfer_graph, *it);
            std::set<node_type> intersection;
            std::set_intersection(first.cbegin(), first.cend(), second.cbegin(), second.cend(),
                        std::inserter(intersection, intersection.begin()));
            double similarity = intersection.size() / std::sqrt(first.size() * second.size());

            std::cerr << node << " <-> " << *it << " : " <<
                  intersection.size() << " / √(" << first.size() << " * " << second.size() << ") = " <<
                  similarity << std::endl;

            if (similarity >= epsilon)
                  core.insert(*it);
      }
      if (core.size() >= mu) return core;
      return {};
      //return (sum > mu) ? core : {};
};

std::unordered_map<node_type, uint32_t> cluster_graph_simple(const transfer_graph_type& transfer_graph, double epsilon, uint32_t mu)
{
      std::unordered_map<node_type, uint32_t> node_ids;
      const uint32_t non_member_id = 0;
      const uint32_t outlier_id = 1;
      const uint32_t hub_id = 2;
      uint32_t last_cluster = 2;

      auto skip_same = [](transfer_graph_type::const_iterator it, transfer_graph_type::const_iterator end) {
            transfer_graph_type::const_iterator last = it++;
            for(; it != end && it->first == last->first; last = it++);
            return it;
      };

      auto it = transfer_graph.begin();
      for(; it != transfer_graph.end(); it = skip_same(it, transfer_graph.end())) {
            //std::cerr << it->first.instance() << " ";
            const auto& node = it->first;
            if (node_ids.find(node) != node_ids.end())
                  continue;

            std::queue<node_type> queue;
            auto core_optional = get_core(transfer_graph, node, epsilon, mu);
            if (core_optional.valid()) {
                  last_cluster++;
                  node_ids[node] = last_cluster;
                  for (const auto& neighbour : *core_optional) {
                        /*auto neighbour_id = node_ids.find(neighbour);
                        if (neighbour_id == node_ids.end()) {
                              node_ids[neighbour] = last_cluster;
                        }*/
                        queue.push(neighbour);
                  }
                  while(!queue.empty()) {
                        auto front = queue.front();
                        queue.pop();
                        auto front_core_optional = get_core(transfer_graph, front, epsilon, mu);
                        if (front_core_optional.valid()) {
                              for (const auto& reachable_node : *front_core_optional) {
                                    auto reachable_id = node_ids.find(reachable_node);
                                    if (reachable_id == node_ids.end()) {
                                          node_ids[reachable_node] = last_cluster;
                                          queue.push(reachable_node);
                                    }
                                    else if (reachable_id->second == non_member_id)
                                          reachable_id->second = last_cluster;
                              }
                        }
                  }
            }
            else {
                  node_ids[node] = non_member_id;
            }
      }
      std::cerr << "!\n";
      for (auto& id : node_ids) {
      //it = transfer_graph.begin();
      //for(; it != transfer_graph.end(); it = skip_same(it, transfer_graph.end())) {
            //std::cerr << id.first.instance() << ' ';
            //auto id = std::make_pair(it->first, node_ids.at(it->first));
            if (id.second == non_member_id) {
                  auto gamma = get_gamma(transfer_graph, id.first);
                  /*bool is_hub = std::inner_product(gamma.begin(), gamma.end(), gamma.begin(), false, std::logical_or<bool>{},
                        [&](const decltype(*gamma.begin())& lhs, const decltype(*gamma.begin())& rhs) {
                              return node_ids.at(lhs) != node_ids.at(rhs);
                  });*/
                  bool is_hub = false;
                  auto g_it1 = gamma.cbegin();
                  auto g_end = gamma.cend();
                  std::cerr << id.first << " -> " << std::flush;
                  for (const auto& gnode : gamma)
                        std::cerr << gnode << ' ' << std::flush;
                  for (; g_it1 != g_end && !is_hub; ++g_it1) {
                        for (auto g_it2 = std::next(g_it1); g_it2 != g_end && !is_hub; ++g_it2) {
                              auto id1 = node_ids.at(*g_it1);
                              auto id2 = node_ids.at(*g_it2);
                              if (id1 > hub_id && id2 > hub_id && id1 != id2)
                                    is_hub = true;
                        }
                  }
                  std::cerr << "!\n";
                  id.second = is_hub ? hub_id : outlier_id;
            }
      }
      std::cerr << "!\n";
      return node_ids;
}

/*return type*/void cluster_graph(const transfer_graph_type& transfer_graph, double epsilon = 0.3, uint32_t mu = 4)
{
      std::unordered_map<node_type, std::unordered_set<uint32_t>> node_ids;
      const uint32_t outlier_id = 0;
      const uint32_t hub_id = 1;
      uint32_t last_cluster = 1;

      const auto compareFirst = [](const transfer_graph_type::value_type& lhs, const transfer_graph_type::value_type& rhs) {
            return lhs.first < rhs.first;
      };

      auto process_core = [&](node_type core_node) {
            auto core_optional = get_core(transfer_graph, core_node, epsilon, mu);
            if (core_optional.valid()) {
                  last_cluster++;
                  for (const auto& neighbour : *core_optional)
                        node_ids[core_node].insert(last_cluster);
            }
            else {
                  node_ids.emplace(core_node, outlier_id);
            }
      };

      auto it = transfer_graph.begin();
      for(; it != transfer_graph.end(); it = /*CHANGE IT !!!*/std::upper_bound(it, transfer_graph.end(), *it, compareFirst)) {
            const auto& node = it->first;
            if (node_ids.find(node) != node_ids.end())
                  continue;

            std::unordered_set<node_type> passed{node};
            process_core(node);
            std::unordered_set<node_type> two_hops_union;
            do {
                  for (const auto& passed_node : passed) {
                        size_t bucket_num = transfer_graph.bucket(passed_node);
                        auto it = transfer_graph.cbegin(bucket_num);
                        auto two_hops_gamma = get_gamma(transfer_graph, it->second);
                        two_hops_union.insert(two_hops_gamma.begin(), two_hops_gamma.end());
                  }
                  for (const auto& passed_node : passed)
                        two_hops_union.erase(passed_node);
                  for (const auto& th_node : two_hops_union)
                        process_core(th_node);
                  passed.insert(two_hops_union.begin(), two_hops_union.end());
            } while(!two_hops_union.empty());
      }
      /*manage multiple cores*/
      /*manage hubs*/
}
#endif

struct get_transfer_accounts_visitor
{
   struct tranfser_accounts_type
   {
      account_id_type from;
      account_id_type to;
      asset amount;
   };

   typedef fc::optional<tranfser_accounts_type> result_type;

   result_type operator()( const transfer_operation& op )
   {

      return result_type{tranfser_accounts_type{op.from, op.to, op.amount}};
   }

   /*result_type operator()( const override_transfer_operation& op )
   {
      return result_type{tranfser_accounts_type{op.from, op.to}}
   }*/

   template <typename Operation>
   result_type operator()( const Operation& op)
   {
      return result_type{};
   }
 };


 transfer_graph_type2 database::construct_transfer_graph(fc::time_point_sec last_maintenance, std::unordered_map<object_id_type, uint32_t>& account_transfers)
{
   /*auto transaction_expiration = get_global_properties().parameters.maximum_time_until_expiration;
   if (last_maintenance > head_block_time() - transaction_expiration) {
      transfer_graph_type transfer_graph;

      const auto& all_transactions = get_index_type<transaction_index>().indices().get<by_trx_id>();
      for (const auto& tr : all_transactions) {
         for ( const auto& op : tr.trx.operations) {
            get_transfer_accounts_visitor vtor;
            auto account_pair = op.visit(vtor);
            if (account_pair.valid()) {
               account_id_type from {account_pair->from};
               account_id_type to   {account_pair->to};

               //std::cout << from(*this).name << " : " << uint64_t(from) << " -> " << to(*this).name << '\n';

               auto pair = std::pair<const object_id_type, object_id_type>(to, from); //not a bug
               if (std::find(transfer_graph.cbegin(), transfer_graph.cend(), pair) == transfer_graph.cend())
                  transfer_graph.insert(std::move(pair));

               ++account_transfers[object_id_type{from}];
            }
         }
      }
      return transfer_graph;
   }*/

   auto optional_block = fetch_block_by_id(head_block_id());
   transfer_graph_type2 transfer_graph;
   size_t blocks = 0;
   auto max_blocks = get_global_properties().parameters.importance_score_block_count;
   asset min_transfer_amount = get_global_properties().parameters.min_transfer_for_importance;

   while(optional_block.valid() && (blocks++ < max_blocks)) {
      auto& block = *optional_block;

      if (last_maintenance > block.timestamp)
         break;

      for (const auto& tr : block.transactions) {
         for ( const auto& op : tr.operations) {
            get_transfer_accounts_visitor vtor;
            auto account_pair = op.visit(vtor);
            if (account_pair.valid() && account_pair->amount > min_transfer_amount) {
               account_id_type from {account_pair->from};
               account_id_type to   {account_pair->to};

               //std::cout << from(*this).name << " : " << uint64_t(from) << " -> " << to(*this).name << '\n';
               auto pair = std::pair<const object_id_type, object_id_type>(to, from); //not a bug
               if (std::find(transfer_graph.cbegin(), transfer_graph.cend(), pair) == transfer_graph.cend())
               transfer_graph.insert(std::move(pair));

               ++account_transfers[object_id_type{from}];
            }
         }
      }

     optional_block = fetch_block_by_id(block.previous);
   }

   return transfer_graph;
}

#if 0
void database::renew_importance_score(fc::time_point_sec last_maintenance)
{
   std::unordered_map<object_id_type, uint32_t> account_transfers;
   auto transfer_graph = construct_transfer_graph(last_maintenance, account_transfers);
   std::unordered_map<object_id_type, uint32_t> account_cycles;

   auto max_blocks = get_global_properties().parameters.importance_score_block_count;

   auto it = transfer_graph.cbegin();
   for (; it != transfer_graph.cend(); ++it) {
      auto cycle_target = it->first;

      uint32_t cycles_count = rec_cycle_search(transfer_graph, it->second, cycle_target, 100);

      account_cycles[it->first] += cycles_count;// ? 1 : 0;
   }

   //std::cout << "Maint\n";
   const auto& all_accounts = get_index_type<account_index>().indices().get<by_id>();
   for (auto& sender_account : all_accounts) {

      auto& sender_statistics = sender_account.statistics(*this);

      auto acc_transfers = account_transfers[object_id_type(sender_account.id)];
      auto acc_cycles    = account_cycles[sender_account.id];

      if (acc_transfers > acc_cycles)
         acc_transfers -= acc_cycles;
      else
         acc_transfers = 0;

      //std::cout << sender_account.name << " : " << uint64_t(sender_account.id) << " = " << acc_transfers << '\n';

      modify(sender_account.statistics(*this), [&](account_statistics_object& s)
      {
         auto& chrono = s.transfers_chronology;
         auto last_score = 0;
         if (!chrono.empty())
            last_score = chrono.rbegin()->second;
         chrono[head_block_num()] = last_score + acc_transfers;

         auto it = chrono.lower_bound(head_block_num() - max_blocks);
         auto minus = 0;
         if (it != chrono.end())
            minus = it->second;

         s.transfer_rate = chrono.rbegin()->second - minus;
      });
   }
}
#endif

void database::perform_chain_maintenance(const signed_block& next_block, const global_property_object& global_props)
{
   const auto& gpo = get_global_properties();

   //renew_importance_score(next_block.timestamp - gpo.parameters.maintenance_interval);
   calculate_importance_score(next_block.timestamp - gpo.parameters.maintenance_interval);

   distribute_fba_balances(*this);
   create_buyback_orders(*this);

   struct vote_tally_helper {
      database& d;
      const global_property_object& props;

      vote_tally_helper(database& d, const global_property_object& gpo)
         : d(d), props(gpo)
      {
         d._vote_tally_buffer.resize(props.next_available_vote_id);
         d._witness_count_histogram_buffer.resize(props.parameters.maximum_witness_count / 2 + 1);
         d._committee_count_histogram_buffer.resize(props.parameters.maximum_committee_count / 2 + 1);
         d._total_importance_score = 0;
      }

      void operator()(const account_object& stake_account) {
         if( props.parameters.count_non_member_votes || stake_account.is_member(d.head_block_time()) )
         {
            // There may be a difference between the account whose stake is voting and the one specifying opinions.
            // Usually they're the same, but if the stake account has specified a voting_account, that account is the one
            // specifying the opinions.
            const account_object& opinion_account =
                  (stake_account.options.voting_account ==
                   GRAPHENE_PROXY_TO_SELF_ACCOUNT)? stake_account
                                     : d.get(stake_account.options.voting_account);

            const auto& stats = stake_account.statistics(d);
            /*uint64_t voting_stake = stats.total_core_in_orders.value
                  + (stake_account.cashback_vb.valid() ? (*stake_account.cashback_vb)(d).balance.amount.value: 0)
                  + d.get_balance(stake_account.get_id(), asset_id_type()).amount.value;  */

            const auto& gpo = d.get_global_properties();
            //auto current_balance = d.get_balance(stake_account.get_id(), asset_id_type()).amount.value;
            //auto balance_multiplier = gpo.parameters.balance_multiplier;

            //uint64_t importance_score = stats.transfer_rate + balance_multiplier * current_balance;

            /*d.modify(stake_account.statistics(d), [&](account_statistics_object& s)
            {
               s.importance_score = importance_score;
            });*/

            uint64_t importance_score = stats.importance_score * 1e6;

            for( vote_id_type id : opinion_account.options.votes )
            {
               uint32_t offset = id.instance();
               // if they somehow managed to specify an illegal offset, ignore it.
               if( offset < d._vote_tally_buffer.size() )
                  d._vote_tally_buffer[offset] += importance_score;
            }

            if( opinion_account.options.num_witness <= props.parameters.maximum_witness_count )
            {
               uint16_t offset = std::min(size_t(opinion_account.options.num_witness/2),
                                          d._witness_count_histogram_buffer.size() - 1);
               // votes for a number greater than maximum_witness_count
               // are turned into votes for maximum_witness_count.
               //
               // in particular, this takes care of the case where a
               // member was voting for a high number, then the
               // parameter was lowered.
               d._witness_count_histogram_buffer[offset] += importance_score;
            }
            if( opinion_account.options.num_committee <= props.parameters.maximum_committee_count )
            {
               uint16_t offset = std::min(size_t(opinion_account.options.num_committee/2),
                                          d._committee_count_histogram_buffer.size() - 1);
               // votes for a number greater than maximum_committee_count
               // are turned into votes for maximum_committee_count.
               //
               // same rationale as for witnesses
               d._committee_count_histogram_buffer[offset] += importance_score;
            }

            d._total_importance_score += importance_score;
         }
      }
   } tally_helper(*this, gpo);
   struct process_fees_helper {
      database& d;
      const global_property_object& props;

      process_fees_helper(database& d, const global_property_object& gpo)
         : d(d), props(gpo) {}

      void operator()(const account_object& a) {
         a.statistics(d).process_fees(a, d);
      }
   } fee_helper(*this, gpo);

   perform_account_maintenance(std::tie(
      tally_helper,
      fee_helper
      ));

   struct clear_canary {
      clear_canary(vector<uint64_t>& target): target(target){}
      ~clear_canary() { target.clear(); }
   private:
      vector<uint64_t>& target;
   };
   clear_canary a(_witness_count_histogram_buffer),
                b(_committee_count_histogram_buffer),
                c(_vote_tally_buffer);

   update_top_n_authorities(*this);
   update_active_witnesses();
   update_active_committee_members();
   update_worker_votes();

   modify(gpo, [this](global_property_object& p) {
      // Remove scaling of account registration fee
      const auto& dgpo = get_dynamic_global_properties();
      p.parameters.current_fees->get<account_create_operation>().basic_fee >>= p.parameters.account_fee_scale_bitshifts *
            (dgpo.accounts_registered_this_interval / p.parameters.accounts_per_fee_scale);

      if( p.pending_parameters )
      {
         p.parameters = std::move(*p.pending_parameters);
         p.pending_parameters.reset();
      }
   });

   auto next_maintenance_time = get<dynamic_global_property_object>(dynamic_global_property_id_type()).next_maintenance_time;
   auto maintenance_interval = gpo.parameters.maintenance_interval;

   if( next_maintenance_time <= next_block.timestamp )
   {
      if( next_block.block_num() == 1 )
         next_maintenance_time = time_point_sec() +
               (((next_block.timestamp.sec_since_epoch() / maintenance_interval) + 1) * maintenance_interval);
      else
      {
         // We want to find the smallest k such that next_maintenance_time + k * maintenance_interval > head_block_time()
         //  This implies k > ( head_block_time() - next_maintenance_time ) / maintenance_interval
         //
         // Let y be the right-hand side of this inequality, i.e.
         // y = ( head_block_time() - next_maintenance_time ) / maintenance_interval
         //
         // and let the fractional part f be y-floor(y).  Clearly 0 <= f < 1.
         // We can rewrite f = y-floor(y) as floor(y) = y-f.
         //
         // Clearly k = floor(y)+1 has k > y as desired.  Now we must
         // show that this is the least such k, i.e. k-1 <= y.
         //
         // But k-1 = floor(y)+1-1 = floor(y) = y-f <= y.
         // So this k suffices.
         //
         auto y = (head_block_time() - next_maintenance_time).to_seconds() / maintenance_interval;
         next_maintenance_time += (y+1) * maintenance_interval;
      }
   }

   const dynamic_global_property_object& dgpo = get_dynamic_global_properties();

   if( (dgpo.next_maintenance_time < HARDFORK_613_TIME) && (next_maintenance_time >= HARDFORK_613_TIME) )
      deprecate_annual_members(*this);

   modify(dgpo, [next_maintenance_time](dynamic_global_property_object& d) {
      d.next_maintenance_time = next_maintenance_time;
      d.accounts_registered_this_interval = 0;
   });

   // process_budget needs to run at the bottom because
   //   it needs to know the next_maintenance_time
   process_budget();
}

} }
