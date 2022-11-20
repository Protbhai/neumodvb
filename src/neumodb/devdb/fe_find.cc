/*
 * Neumo dvb (C) 2019-2022 deeptho@gmail.com
 *
 * Copyright notice:
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#include "neumodb/chdb/chdb_extra.h"
#include "receiver/neumofrontend.h"
#include "stackstring/ssaccu.h"
#include "xformat/ioformat.h"
#include <iomanip>
#include <iostream>
#include <signal.h>

#include "../util/neumovariant.h"

using namespace devdb;

/*
	returns the remaining use_count of the unsuscribed fe
 */
int fe::unsubscribe(db_txn& wtxn, const fe_key_t& fe_key, fe_t* fe_ret) {
	auto c = devdb::fe_t::find_by_key(wtxn, fe_key);
	assert(c.is_valid());
	auto fe = c.current(); //update in case of external changes
	dtdebugx("adapter %d %d%c-%d use_count=%d\n", fe.adapter_no, fe.sub.frequency/1000,
					 fe.sub.pol == chdb::fe_polarisation_t::H ? 'H': 'V', fe.sub.stream_id, fe.sub.use_count);
	assert(fe.sub.use_count>=1);
	if(--fe.sub.use_count == 0) {
		fe.sub = {};
	}
	put_record(wtxn, fe);
	if (fe_ret)
		*fe_ret = fe;
	return fe.sub.use_count;
}

/*
	returns the remaining use_count of the unsubscribed fe
 */
int fe::unsubscribe(db_txn& wtxn, fe_t& fe) {
	assert(fe::is_subscribed(fe));
	assert(fe.sub.lnb_key.card_mac_address != -1);
	auto ret = fe::unsubscribe(wtxn, fe.k, &fe);
	return ret;
}

std::tuple<devdb::fe_t, int> fe::subscribe_fe_in_use(db_txn& wtxn, const fe_key_t& fe_key,
																										 const devdb::fe_key_t* fe_key_to_release) {
	auto c = devdb::fe_t::find_by_key(wtxn, fe_key);
	auto fe = c.is_valid()  ? c.current() : fe_t{}; //update in case of external changes
	int released_fe_usecount{0};
	assert(fe.sub.use_count>=1);
	++fe.sub.use_count;
	dtdebugx("adapter %d %d%c-%d use_count=%d\n", fe.adapter_no, fe.sub.frequency/1000,
					 fe.sub.pol == chdb::fe_polarisation_t::H ? 'H': 'V', fe.sub.stream_id, fe.sub.use_count);

	if(fe_key_to_release)
		released_fe_usecount = unsubscribe(wtxn, *fe_key_to_release);

	put_record(wtxn, fe);
	return {fe, released_fe_usecount};
}


std::optional<devdb::fe_t> fe::find_best_fe_for_dvtdbc(db_txn& rtxn,
																											const devdb::fe_key_t* fe_to_release,
																											bool need_blindscan,
																											bool need_spectrum,
																											bool need_multistream,
																											chdb::delsys_type_t delsys_type) {
	bool need_dvbt = delsys_type == chdb::delsys_type_t::DVB_T;
	bool need_dvbc = delsys_type == chdb::delsys_type_t::DVB_C;
	auto c = devdb::find_first<devdb::fe_t>(rtxn);

	fe_t best_fe{}; //the fe that we will use
	best_fe.priority = std::numeric_limits<decltype(best_fe.priority)>::lowest();

	auto no_best_fe_yet = [&best_fe]()
		{ return best_fe.priority == std::numeric_limits<decltype(best_fe.priority)>::lowest(); };

	auto adapter_in_use = [&rtxn](int adapter_no) {
		auto c = fe_t::find_by_adapter_no(rtxn, adapter_no);
		for(const auto& fe: c.range()) {
			if(fe::is_subscribed(fe))
				return true;
		}
		return false;
	};

	for(const auto& fe: c.range()) {
		if (need_dvbc && (!fe.enable_dvbc || !fe::suports_delsys_type(fe, chdb::delsys_type_t::DVB_C)))
			continue;
		if (need_dvbt && (!fe.enable_dvbt || !fe::suports_delsys_type(fe, chdb::delsys_type_t::DVB_T)))
			continue;

		if((!fe::is_subscribed(fe) && ! adapter_in_use(fe.adapter_no)) ||
			 (fe_to_release && fe.k == *fe_to_release) //consider the future case where the fe will be unsubscribed
			) {
			//find the best fe with all required functionality, without taking into account other subscriptions
			if(!fe.present || ! fe.can_be_used)
				continue; 			/* we can use this frontend only if it can be connected to the proper input*/

			//this fe is one that we can potentially use

			if(need_blindscan && !fe.supports.blindscan)
				continue;
			if(need_multistream && !fe.supports.multistream)
				continue;

			if(need_spectrum) {
				assert (no_best_fe_yet() || best_fe.supports.spectrum_fft || best_fe.supports.spectrum_sweep);

				if(fe.supports.spectrum_fft) { //best choice
					if(no_best_fe_yet() ||
						 !best_fe.supports.spectrum_fft || //fft is better
						 fe.priority > best_fe.priority)
						best_fe = fe;
				} else if (fe.supports.spectrum_sweep) { //second best choice
					if( no_best_fe_yet() ||
							( !best_fe.supports.spectrum_fft && //best_fe with fft beats fe without fft
							 fe.priority > best_fe.priority ))
						best_fe = fe;
				} else {
         //no spectrum support at all -> not useable
				}
			} else { /* if !need_spectrum; in this case fe's with and without fft support can be
									used, but we prefer one without fft, to keep fft-hardware available for other
									subscriptions*/
				if(no_best_fe_yet() ||
					 (best_fe.supports.spectrum_fft && !fe.supports.spectrum_fft) || //prefer non-fft
					 (best_fe.supports.spectrum_sweep && !fe.supports.spectrum_fft
						&& !fe.supports.spectrum_sweep) || //prefer fe with least unneeded functionality
					 fe.priority > best_fe.priority )
					best_fe = fe;
			} //end of !need_spectrum
			continue;
		} //end of !is_subscribed (fe)

		//this fe has an active subscription and it is not our own subscription
	}
	if (no_best_fe_yet())
		return {};
	return best_fe;
}


//how many active fe_t's use lnb?
devdb::resource_subscription_counts_t
devdb::fe::subscription_counts(db_txn& rtxn, const lnb_key_t& lnb_key, const devdb::fe_key_t* fe_key_to_release) {
	devdb::resource_subscription_counts_t ret;
	auto c = fe_t::find_by_card_mac_address(rtxn, lnb_key.card_mac_address, find_type_t::find_eq,
																					fe_t::partial_keys_t::card_mac_address);
	auto rf_coupler_id = lnb::rf_coupler_id(rtxn, lnb_key);

	for(const auto& fe: c.range()) {
		if( !fe::is_subscribed(fe))
			continue;
		if(fe.sub.owner != getpid() && kill((pid_t)fe.sub.owner, 0)) {
			dtdebugx("process pid=%d has died\n", fe.sub.owner);
			continue;
		}
		bool fe_will_be_released = fe_key_to_release && *fe_key_to_release == fe.k;
		if(!fe_will_be_released) {
			if(fe.sub.lnb_key == lnb_key )
				ret.lnb++;
			if(fe.sub.lnb_key.rf_input == lnb_key.rf_input)
				ret.tuner++;
			if(lnb::rf_coupler_id(rtxn, fe.sub.lnb_key) == rf_coupler_id)
				ret.rf_coupler++;
			if( fe.sub.lnb_key.dish_id == lnb_key.dish_id
					|| fe.sub.lnb_key == lnb_key) //the last test is in case dish_id is set to -1
				ret.dish++;
		}
	}
	return ret;
}

/*
	Find out if the desired lnb can be subscribed and then switched to the desired band and polarisation
	and find a frontend that can be used with this lnb
	The rules are:
	-if a result is found, then the user can subscribe the lnb and frontend and can also switch the rf_mux
	 to connect the lnb to the frontend
  -if an adapter has mulitple frontends, then in dvbapi this means that the frontends share the demod
	 and that multiple frontends on the same adapter cannot be used simultaneously (tbs 6504).
	 Most multi-standard (dvbs+c+t) devices provide two frontends because typically two tuners are needed:
   one for dvbs and one for dvbc+t. Devices with only dvbc+t tend to use one tuner for both standards and
	 thus have one frontend
	-if need_blindscan/need_spectrum/need_multistream==true, then the returned fe will be able to
	blindscan/spectrumscan/multistream

	-pol == NONE or band==NONE or usals_pos == sat_pos_none,
	 then the subscription is exclusive: no other subscription
	 will be able to send diseqc commands to pick a different lnb, or polarisation or band

	-if pol != NONE or band!=NONE or sat_pos!=sat_pos_none, then the subscription is
   non-exclusive: other subscribers can reserve another frontend to use the same LNB.
   None of the  subscribers	can send diseqc command to pick a differen lnb, or to rotate a dish or
	 change polarisation or band, except if they are the only subscriber left to this lnb

	 Note that this function does not make the subscription. The caller can first investigate alternatives (e.g.,
	 using other LNBs) and then make a final decision. By using a read/write database transaction the result
	 will be atomic
 */


std::optional<devdb::fe_t> fe::find_best_fe_for_lnb(db_txn& rtxn, const devdb::lnb_t& lnb,
																									 const devdb::fe_key_t* fe_key_to_release,
																									 bool need_blindscan, bool need_spectrum,
																									 bool need_multistream,
																									 chdb::fe_polarisation_t pol, fe_band_t band, int usals_pos) {

	//TODO: clean subscriptions at startup
	auto rf_coupler_id = devdb::lnb::rf_coupler_id(rtxn, lnb.k);
	auto lnb_on_positioner = devdb::lnb::on_positioner(lnb);

	bool need_exclusivity = pol == chdb::fe_polarisation_t::NONE ||
		band == devdb::fe_band_t::NONE || usals_pos == sat_pos_none;

	fe_t best_fe{}; //the fe that we will use
	best_fe.priority = std::numeric_limits<decltype(best_fe.priority)>::lowest();

	auto no_best_fe_yet = [&best_fe]()
		{ return best_fe.priority == std::numeric_limits<decltype(best_fe.priority)>::lowest(); };

	/*
		One adapter can have multiple frontends, and therefore multiple fe_t records.
		We must check all of them
	 */
	auto adapter_in_use = [&rtxn](int adapter_no) {
		auto c = fe_t::find_by_adapter_no(rtxn, adapter_no, find_type_t::find_eq, devdb::fe_t::partial_keys_t::adapter_no);
		for(const auto& fe: c.range()) {
			assert(fe.adapter_no  == adapter_no);
			if(!fe.present)
				continue;
			if(fe::is_subscribed(fe))
				return true;
		}
		return false;
	};



	/*
		cables to one rf_input (tuner) on one card can be shared with another rf_input (tuner) on the same or another
		card using external switches, which pass dc power and diseqc commands. Some witches are symmetric, i.e.,
		let thorugh diseqc  from both connectors, others are priority switches, with only one connector being able
		to send diseqc commandswhen the connector with priority has dc power connected.

		Bot the types of input sharing are made known to neumoDVB by declaring a tuner_group>=0 to
		each connected rf_input. Tuners in the same group can be used simulataneoulsy but only on the same
		sat, pol, band combination.

		Note that priority swicthes do not need to be treated specifically as neumoDVB
		never neither of the connectors to send diseqc, except initially when both connectors are idle.
	 */
	auto shared_rf_input_conflict = [&rtxn, pol, band, usals_pos] (const devdb::fe_t& fe, int rf_coupler_id) {
		if(rf_coupler_id <0)
			return false; //not on switch; no conflict possible
		if(devdb::lnb::rf_coupler_id(rtxn, fe.sub.lnb_key) != rf_coupler_id)
			return false;  //no conflict possible
		if(!fe.enable_dvbs || !devdb::fe::suports_delsys_type(fe, chdb::delsys_type_t::DVB_S))
			return false; //no conflict possible (not sat)
		if(fe.sub.pol != pol || fe.sub.band != band || fe.sub.usals_pos != usals_pos)
			return true;
		return false;
	};

	/*
		multiple LNBs can be in use on the same dish. In this case only the first
		subscription should be able to move the dish. If any subcription uses the required dish_id,
		we need to check if the dish is tuned to the right position.
		Furthermore,m if we need exclusivity, we cannot use the dish at all if a subscription exists

	 */
	auto shared_positioner_conflict = [usals_pos, need_exclusivity] (const devdb::fe_t& fe, int dish_id) {
		if (dish_id <0)
			return false; //lnb is on a dish of its own (otherwise user needs to set dish_id)
		if (fe.sub.lnb_key.dish_id <0 || fe.sub.lnb_key.dish_id != dish_id )
			return false; //fe's subscribed lnb is on a dish of its own (otherwise user needs to set dish_id)
		assert(fe.sub.lnb_key.dish_id == dish_id);
		return need_exclusivity || //exclusivity cannot be offered
			(fe.sub.usals_pos == sat_pos_none) ||  //dish is reserved exclusively by fe
			std::abs(usals_pos - fe.sub.usals_pos) >=30; //dish would need to be moved more than 0.3 degree
	};

	auto c = fe_t::find_by_card_mac_address(rtxn, lnb.k.card_mac_address, find_type_t::find_eq,
																					fe_t::partial_keys_t::card_mac_address);
	for(const auto& fe: c.range()) {
		assert(fe.card_mac_address == lnb.k.card_mac_address);
		assert(fe.sub.lnb_key.rf_input != lnb.k.rf_input || fe.sub.lnb_key == lnb.k);
		bool is_subscribed = fe::is_subscribed(fe);
		bool is_our_subscription = (fe_key_to_release && fe.k == *fe_key_to_release);
		if(!is_subscribed || is_our_subscription) {
      //find the best fe will all required functionality, without taking into account other subscriptions
			if(!fe.present || !devdb::fe::suports_delsys_type(fe, chdb::delsys_type_t::DVB_S))
				continue; /* The fe does not currently exist, or it cannot use DVBS. So it
									 can also not create conflicts with other fes*/

			if(! fe.enable_dvbs) //disabled by user
				continue; /*there could still be conflicts with external users (other programs)
										which could impact usage of lnb, but not with other neumoDVB instances,
										as they should also see the same value of fe.enable_dvbs
									*/
#if 0
			if(!fe.can_be_used)
			{/*noop*/}  /* We cannot open the fe and control it, which means another program
										 has control. This other program must be a separare instance of neumoDVB;
										 otherwise there can be no subscription.
									*/
#endif
			if( !fe.rf_inputs.contains(lnb.k.rf_input) ||
					(need_blindscan && !fe.supports.blindscan) ||
					(need_multistream && !fe.supports.multistream)
				)
				continue;  /* we cannot use the LNB with this fe, and as it is not subscribed it
											conflicts for using the LNB with other fes => so no "continue"
									 */

			if(!is_our_subscription && adapter_in_use(fe.adapter_no))
				continue; /*adapter is on use for dvbc/dvt; it cannot be used, but the
										fe we found is not in use and cannot create conflicts with any other frontends*/

			if(need_spectrum) {
				assert (no_best_fe_yet() || best_fe.supports.spectrum_fft || best_fe.supports.spectrum_sweep);

				if(fe.supports.spectrum_fft) { //best choice
					if(no_best_fe_yet() ||
						 !best_fe.supports.spectrum_fft || //fft is better
						 (fe.priority > best_fe.priority ||
							(fe.priority == best_fe.priority && is_our_subscription)) //prefer current adapter
						)
						best_fe = fe;
					} else if (fe.supports.spectrum_sweep) { //second best choice
					if( no_best_fe_yet() ||
							( !best_fe.supports.spectrum_fft && //best_fe with fft beats fe without fft
								fe.priority > best_fe.priority ))
						best_fe = fe;
				} else {
					//no spectrum support at all -> not useable
				}
			} else { /* if !need_spectrum; in this case fe's with and without fft support can be
										used, but we prefer one without fft, to keep fft-hardware available for other
										subscriptions*/
				if(no_best_fe_yet() ||
					 (best_fe.supports.spectrum_fft && !fe.supports.spectrum_fft) || //prefer non-fft
					 (best_fe.supports.spectrum_sweep && !fe.supports.spectrum_fft
						&& !fe.supports.spectrum_sweep) || //prefer fe with least unneeded functionality

					 (fe.priority > best_fe.priority ||
						(fe.priority == best_fe.priority && is_our_subscription)) //prefer current adapter
					)
					best_fe = fe;
			} //end of !need_spectrum
			continue;
		} //end of !is_subscribed

		assert(is_subscribed && !is_our_subscription);

		/*this fe has an active subscription and it is not our own subscription.
			The resources it uses (RF tuner, priority switches or t-splitters, band/pol/usals_pos)
			could conflict with the lnb we want to use. We check for all possible conflicts
		*/
		if(fe.sub.lnb_key == lnb.k) {
      /*case 1: fe uses our lnb for another subscription; associated RF tuner is also in use
				Check if our desired (non)exclusivity matches with other subscribers */
			if(need_exclusivity)
				return {}; //only one subscriber can exclusively control the lnb (and the path to it)
			else { /*we do not need exclusivity, and can use this lnb  provided no exclusive subscriptions exist
							 and provided that the lnb parameters (pol/band/diseqc) are compatible*/
				if(fe.sub.pol != pol || fe.sub.band != band || fe.sub.usals_pos != usals_pos)
					return {}; /*parameter do not match, or some other subscription is exclusive (in the latter
													case pol or band will be NONE or usals_pos will be sat_pos_none and the test will
													be true;
										 */
			}
		} else if (fe.sub.lnb_key.rf_input == lnb.k.rf_input) {
      /*case 2: fe does not use our desired lnb, but uses the RF tuner with some other lnb.
				We cannot use the RF tuner, so we can not use the LNB*/
			return {};
		} else {
			/* case 3. the desired LNB and the desired RF tuner are not used by fe

				 The remain conflicts are:
				 1) our lnb is connected through a priority or T-combiner switch,
				    in which case the cable may use another lnb (note the case of the same lnb, but wrong pol/band/sat_pos
				    has already been handled in case 1) and we cannot use it
				 2) our lnb is on a dish with a positioner, and this actuve frontend uses another lnb on the same dish
				    pointing to a different sat
			 */
			if (rf_coupler_id >=0 && shared_rf_input_conflict (fe, rf_coupler_id))
				return {}; //the lnb is on a cable which is tuned to another sat/pol/band

			if (lnb_on_positioner && shared_positioner_conflict (fe, lnb.k.dish_id))
				return {}; //the lnb is on a cable which is tuned to another sat/pol/band
		}
	}


	if (no_best_fe_yet())
		return {};
	return best_fe;
}

/*
	Return the use_counts as they will be after releasing fe_key_to_release
 */
std::tuple<std::optional<devdb::fe_t>, std::optional<devdb::lnb_t>, devdb::resource_subscription_counts_t>
fe::find_fe_and_lnb_for_tuning_to_mux(db_txn& rtxn,
																			const chdb::dvbs_mux_t& mux, const devdb::lnb_key_t* required_lnb_key,
																			const devdb::fe_key_t* fe_key_to_release,
																			bool may_move_dish, bool use_blind_tune,
																			int dish_move_penalty, int resource_reuse_bonus) {
	using namespace devdb;
	int best_lnb_prio = std::numeric_limits<int>::min();
	int best_fe_prio = std::numeric_limits<int>::min();
	// best lnb sofar, and the corresponding connected frontend
	std::optional<devdb::lnb_t> best_lnb;
	std::optional<devdb::fe_t> best_fe;
	resource_subscription_counts_t best_use_counts;

	/*
		Loop over all lnbs to find a suitable one.
		In the loop below, check if the lnb is compatible with the desired mux and tune options.
		If the lnb is compatible, check check all existing subscriptions for conflicts.
	*/
	auto c = required_lnb_key ? lnb_t::find_by_key(rtxn, *required_lnb_key, find_type_t::find_eq,
																						 devdb::lnb_t::partial_keys_t::all)
		: find_first<devdb::lnb_t>(rtxn);
	for (auto const& lnb : c.range()) {
		assert(! required_lnb_key || *required_lnb_key == lnb.k);
		if(!lnb.enabled || !lnb.can_be_used)
			continue;
		/*
			required_lnb may not have been saved in the database and may contain additional networks or
			edited settings when called from positioner_dialog
		*/
		auto [has_network, network_priority, usals_move_amount, usals_pos] = devdb::lnb::has_network(lnb, mux.k.sat_pos);
		/*priority==-1 indicates:
			for lnb_network: lnb.priority should be consulted
			for lnb: front_end.priority should be consulted
		*/
		auto dish_needs_to_be_moved_ = usals_move_amount != 0;
		bool lnb_can_control_rotor = devdb::lnb::can_move_dish(lnb);
		bool lnb_is_on_rotor = devdb::lnb::on_positioner(lnb);

		if (lnb_is_on_rotor && (usals_move_amount >= 30) &&
				(!may_move_dish || ! lnb_can_control_rotor)
			)
			continue; //skip because dish movement is not allowed or  not possible

		auto lnb_priority = network_priority >= 0 ? network_priority : lnb.priority;
		auto penalty = dish_needs_to_be_moved_ ? dish_move_penalty : 0;
		if (!has_network ||
				(lnb_priority >= 0 && lnb_priority - penalty < best_lnb_prio) //we already have a better fe
			)
			continue;

    /* check if lnb  support required frequency, polarisation...*/
		const bool disregard_networks{false};
		if (!devdb::lnb_can_tune_to_mux(lnb, mux, disregard_networks))
			continue;


		const auto need_blindscan = use_blind_tune;
		const bool need_spectrum = false;
		auto pol{mux.pol}; //signifies that non-exlusive control is fine
		auto band{devdb::lnb::band_for_mux(lnb, mux)}; //signifies that non-exlusive control is fine

		bool need_multistream = (mux.stream_id >= 0);
		auto fe = fe::find_best_fe_for_lnb(rtxn, lnb, fe_key_to_release,
																				 need_blindscan, need_spectrum, need_multistream, pol, band, usals_pos);
		if(!fe) {
			dtdebug("LNB " << lnb << " cannot be used");
			continue;
		}
		auto fe_prio = fe->priority;
		auto use_counts = subscription_counts(rtxn, lnb.k, fe_key_to_release);

		if(use_counts.lnb >= 1 ||
			 use_counts.tuner >= 1 ||
			 use_counts.dish >=1 ||
			 use_counts.rf_coupler >=1)
			fe_prio += resource_reuse_bonus;




		if (lnb_priority < 0 || lnb_priority - penalty == best_lnb_prio)
			if (fe_prio - penalty <= best_fe_prio) // use fe_priority to break the tie
				continue;

		/*we cannot move the dish, but we can still use this lnb if the dish
				happens to be pointint to the correct sat
		*/
		best_fe_prio = fe_prio - penalty;
		best_lnb_prio = (lnb_priority < 0 ? fe_prio : lnb_priority) - penalty; //<0 means: use fe_priority
		best_lnb = lnb;
		best_fe = fe;
		best_use_counts = use_counts;
		if (required_lnb_key)
			break; //we only beed to look at one lnb
	}
	return std::make_tuple(best_fe, best_lnb, best_use_counts);
}

int devdb::fe::reserve_fe_lnb_band_pol_sat(db_txn& wtxn, devdb::fe_t& fe, const devdb::lnb_t& lnb,
																					 devdb::fe_band_t band,  chdb::fe_polarisation_t pol,
																					 int frequency, int stream_id)
{
	auto c = devdb::fe_t::find_by_key(wtxn, fe.k);
	if( !c.is_valid())
		return -1;
	fe = c.current();
	auto& sub = fe.sub;
	assert(sub.use_count == 0);
	sub.owner = getpid();
	assert(sub.use_count==0);
	sub.use_count = 1;
	//the following settings imply that we request a non-exclusive subscription
	sub.lnb_key = lnb.k;
	sub.pol = pol;
	sub.band = band;
	sub.usals_pos = lnb.usals_pos;
	sub.frequency = frequency; //for informational purposes
	sub.stream_id = stream_id; //for informational purposes
	dtdebugx("adapter %d %d%c-%d use_count=%d\n", fe.adapter_no, fe.sub.frequency/1000,
					 fe.sub.pol == chdb::fe_polarisation_t::H ? 'H': 'V', fe.sub.stream_id, fe.sub.use_count);
	put_record(wtxn, fe);
	return 0;
}

int devdb::fe::reserve_fe_lnb_exclusive(db_txn& wtxn, devdb::fe_t& fe, const devdb::lnb_t& lnb)
{
	auto c = devdb::fe_t::find_by_key(wtxn, fe.k);
	if( !c.is_valid())
		return -1;
	fe = c.current();
	auto& sub = fe.sub;
	assert(sub.use_count == 0);
	sub.owner = getpid();
	assert(sub.use_count==0);
	sub.use_count = 1;
	//the following settings imply that we request a non-exclusive subscription
	sub.lnb_key = lnb.k;
	sub.pol = chdb::fe_polarisation_t::NONE;
	sub.band = devdb::fe_band_t::NONE;
	sub.usals_pos = sat_pos_none;
	sub.frequency = 0;
		sub.stream_id = -1;
	dtdebugx("adapter %d %d%c-%d use_count=%d\n", fe.adapter_no, fe.sub.frequency/1000,
					 fe.sub.pol == chdb::fe_polarisation_t::H ? 'H': 'V', fe.sub.stream_id, fe.sub.use_count);
	put_record(wtxn, fe);
	return 0;
}

int devdb::fe::reserve_fe_dvbc_or_dvbt_mux(db_txn& wtxn, devdb::fe_t& fe, bool is_dvbc, int frequency, int stream_id)
{
	auto c = devdb::fe_t::find_by_key(wtxn, fe.k);
	if( !c.is_valid())
		return -1;
	fe = c.current();
	auto& sub = fe.sub;
	assert(sub.use_count == 0);
	sub.owner = getpid();
	assert(sub.use_count==0);
	sub.use_count = 1;
	//the following settings imply that we request a non-exclusive subscription
	sub.lnb_key = devdb::lnb_key_t{};
	sub.pol = chdb::fe_polarisation_t::NONE;
	sub.band = devdb::fe_band_t::NONE;
	sub.usals_pos = is_dvbc ? sat_pos_dvbc : sat_pos_dvbt;
	sub.frequency = frequency; //for informational purposes only
	sub.stream_id = stream_id; //for informational purposes only
	put_record(wtxn, fe);
	return 0;
}


/*
	returns
	std::optional<devdb::fe_t>: the newly subscribed fe
	int: the use_count after releasing fe_kkey_to_release, or 0 if fe_key_to_release==nullptr
 */
std::tuple<std::optional<devdb::fe_t>, int>
devdb::fe::subscribe_lnb_exclusive(db_txn& wtxn,  const devdb::lnb_t& lnb, const devdb::fe_key_t* fe_key_to_release,
												bool need_blind_tune, bool need_spectrum) {
	auto pol{chdb::fe_polarisation_t::NONE}; //signifies that we to exclusively control pol
	auto band{fe_band_t::NONE}; //signifies that we to exclusively control band
	auto usals_pos{sat_pos_none}; //signifies that we want to be able to move rotor
	bool need_multistream = false;
	int released_fe_usecount{0};

	auto best_fe = fe::find_best_fe_for_lnb(wtxn, lnb, fe_key_to_release,
																					need_blind_tune, need_spectrum, need_multistream, pol, band, usals_pos);
	if(fe_key_to_release)
		released_fe_usecount = unsubscribe(wtxn, *fe_key_to_release);

	if(!best_fe)
		return {}; //no frontend could be found

	auto ret = devdb::fe::reserve_fe_lnb_exclusive(wtxn, *best_fe, lnb);
	assert(ret==0); //reservation cannot fail as we have a write lock on the db
	return {best_fe, released_fe_usecount};
}


/*
	returns
	std::optional<devdb::fe_t>: the newly subscribed fe
	std::optional<devdb::lnb_t>: the newly subscribed lnb
	devdb::resource_subscription_counts_t
	int: the use_count after releasing fe_kkey_to_release, or 0 if fe_key_to_release==nullptr
 */
std::tuple<std::optional<devdb::fe_t>, std::optional<devdb::lnb_t>, devdb::resource_subscription_counts_t, int>
devdb::fe::subscribe_lnb_band_pol_sat(db_txn& wtxn, const chdb::dvbs_mux_t& mux,
													 const devdb::lnb_key_t* required_lnb_key, const devdb::fe_key_t* fe_key_to_release,
																			bool use_blind_tune, bool may_move_dish,
																			int dish_move_penalty, int resource_reuse_bonus) {
	int released_fe_usecount{0};
	auto[best_fe, best_lnb, best_use_counts] =
		fe::find_fe_and_lnb_for_tuning_to_mux(wtxn, mux, required_lnb_key,
																					fe_key_to_release,
																					may_move_dish, use_blind_tune,
																					dish_move_penalty, resource_reuse_bonus);
	if(fe_key_to_release)
		released_fe_usecount = unsubscribe(wtxn, *fe_key_to_release);

	if(!best_fe)
		return {}; //no frontend could be found
	auto ret = devdb::fe::reserve_fe_lnb_band_pol_sat(wtxn, *best_fe, *best_lnb, devdb::lnb::band_for_mux(*best_lnb, mux),
																										mux.pol, mux.frequency, mux.stream_id);
	best_use_counts.dish++;
	best_use_counts.lnb++;
	best_use_counts.rf_coupler++;
	best_use_counts.tuner++;
	assert(ret==0); //reservation cannot fail as we have a write lock on the db
	return {best_fe, best_lnb, best_use_counts, released_fe_usecount};
}


/*
	returns
	std::optional<devdb::fe_t>: the newly subscribed fe
	int: the use_count after releasing fe_kkey_to_release, or 0 if fe_key_to_release==nullptr
 */
template<typename mux_t>
std::tuple<std::optional<devdb::fe_t>, int>
devdb::fe::subscribe_dvbc_or_dvbt_mux(db_txn& wtxn, const mux_t& mux, const devdb::fe_key_t* fe_key_to_release,
									 bool use_blind_tune) {

	const bool need_spectrum{false};
	int released_fe_usecount{0};
	const bool need_multistream = (mux.stream_id >= 0);
	const auto delsys_type = chdb::delsys_type_for_mux_type<mux_t>();
	bool is_dvbc = delsys_type == chdb::delsys_type_t::DVB_C;
	auto best_fe = devdb::fe::find_best_fe_for_dvtdbc(wtxn, fe_key_to_release, use_blind_tune,
																							 need_spectrum, need_multistream,  delsys_type);
	if(fe_key_to_release)
		released_fe_usecount = unsubscribe(wtxn, *fe_key_to_release);

	if(!best_fe)
		return {}; //no frontend could be found

	auto ret = devdb::fe::reserve_fe_dvbc_or_dvbt_mux(wtxn, *best_fe, is_dvbc, mux.frequency, mux.stream_id);
	assert(ret>0); //reservation cannot fail as we have a write lock on the db
	return {best_fe, released_fe_usecount};
}

bool devdb::fe::is_subscribed(const fe_t& fe) {
	if (fe.sub.owner < 0)
		return false;
	if( kill((pid_t)fe.sub.owner, 0)) {
		dtdebugx("process pid=%d has died\n", fe.sub.owner);
		return false;
	}
	return true;
}


//instantiation
template std::tuple<std::optional<devdb::fe_t>, int>
devdb::fe::subscribe_dvbc_or_dvbt_mux<chdb::dvbc_mux_t>(db_txn& wtxn, const chdb::dvbc_mux_t& mux,
																						 const devdb::fe_key_t* fe_key_to_release,
																						 bool use_blind_tune);

template std::tuple<std::optional<devdb::fe_t>, int>
devdb::fe::subscribe_dvbc_or_dvbt_mux<chdb::dvbt_mux_t>(db_txn& wtxn, const chdb::dvbt_mux_t& mux,
																						 const devdb::fe_key_t* fe_key_to_release,
																						 bool use_blind_tune);
