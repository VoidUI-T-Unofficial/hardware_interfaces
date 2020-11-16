/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "GnssHalTestCases"

#include <android/hardware/gnss/IGnssPsds.h>
#include "gnss_hal_test.h"

using android::sp;
using android::hardware::gnss::BlocklistedSource;
using GnssConstellationTypeAidl = android::hardware::gnss::GnssConstellationType;
using android::hardware::gnss::IGnssConfiguration;
using android::hardware::gnss::IGnssPsds;
using android::hardware::gnss::PsdsType;

/*
 * SetupTeardownCreateCleanup:
 * Requests the gnss HAL then calls cleanup
 *
 * Empty test fixture to verify basic Setup & Teardown
 */
TEST_P(GnssHalTest, SetupTeardownCreateCleanup) {}

/*
 * TestPsdsExtension:
 * 1. Gets the PsdsExtension and verifies that it returns a non-null extension.
 * 2. Injects empty PSDS data and verifies that it returns false.
 */
TEST_P(GnssHalTest, TestPsdsExtension) {
    sp<IGnssPsds> iGnssPsds;
    auto status = aidl_gnss_hal_->getExtensionPsds(&iGnssPsds);
    ASSERT_TRUE(status.isOk());
    ASSERT_TRUE(iGnssPsds != nullptr);

    status = iGnssPsds->injectPsdsData(PsdsType::LONG_TERM, std::vector<uint8_t>());
    ASSERT_FALSE(status.isOk());
}

/*
 * FindStrongFrequentNonGpsSource:
 *
 * Search through a GnssSvStatus list for the strongest non-GPS satellite observed enough times
 *
 * returns the strongest source,
 *         or a source with constellation == UNKNOWN if none are found sufficient times
 */
BlocklistedSource FindStrongFrequentNonGpsSource(
        const std::list<hidl_vec<IGnssCallback_2_1::GnssSvInfo>> sv_info_list,
        const int min_observations) {
    struct ComparableBlocklistedSource {
        BlocklistedSource id;

        ComparableBlocklistedSource() {
            id.constellation = GnssConstellationTypeAidl::UNKNOWN;
            id.svid = 0;
        }

        bool operator<(const ComparableBlocklistedSource& compare) const {
            return ((id.svid < compare.id.svid) || ((id.svid == compare.id.svid) &&
                                                    (id.constellation < compare.id.constellation)));
        }
    };

    struct SignalCounts {
        int observations;
        float max_cn0_dbhz;
    };

    std::map<ComparableBlocklistedSource, SignalCounts> mapSignals;

    for (const auto& sv_info_vec : sv_info_list) {
        for (uint32_t iSv = 0; iSv < sv_info_vec.size(); iSv++) {
            const auto& gnss_sv = sv_info_vec[iSv];
            if ((gnss_sv.v2_0.v1_0.svFlag & IGnssCallback_1_0::GnssSvFlags::USED_IN_FIX) &&
                (gnss_sv.v2_0.constellation != GnssConstellationType::GPS)) {
                ComparableBlocklistedSource source;
                source.id.svid = gnss_sv.v2_0.v1_0.svid;
                source.id.constellation =
                        static_cast<GnssConstellationTypeAidl>(gnss_sv.v2_0.constellation);

                const auto& itSignal = mapSignals.find(source);
                if (itSignal == mapSignals.end()) {
                    SignalCounts counts;
                    counts.observations = 1;
                    counts.max_cn0_dbhz = gnss_sv.v2_0.v1_0.cN0Dbhz;
                    mapSignals.insert(
                            std::pair<ComparableBlocklistedSource, SignalCounts>(source, counts));
                } else {
                    itSignal->second.observations++;
                    if (itSignal->second.max_cn0_dbhz < gnss_sv.v2_0.v1_0.cN0Dbhz) {
                        itSignal->second.max_cn0_dbhz = gnss_sv.v2_0.v1_0.cN0Dbhz;
                    }
                }
            }
        }
    }

    float max_cn0_dbhz_with_sufficient_count = 0.;
    int total_observation_count = 0;
    int blocklisted_source_count_observation = 0;

    ComparableBlocklistedSource source_to_blocklist;  // initializes to zero = UNKNOWN constellation
    for (auto const& pairSignal : mapSignals) {
        total_observation_count += pairSignal.second.observations;
        if ((pairSignal.second.observations >= min_observations) &&
            (pairSignal.second.max_cn0_dbhz > max_cn0_dbhz_with_sufficient_count)) {
            source_to_blocklist = pairSignal.first;
            blocklisted_source_count_observation = pairSignal.second.observations;
            max_cn0_dbhz_with_sufficient_count = pairSignal.second.max_cn0_dbhz;
        }
    }
    ALOGD("Among %d observations, chose svid %d, constellation %d, "
          "with %d observations at %.1f max CNo",
          total_observation_count, source_to_blocklist.id.svid,
          (int)source_to_blocklist.id.constellation, blocklisted_source_count_observation,
          max_cn0_dbhz_with_sufficient_count);

    return source_to_blocklist.id;
}

/*
 * BlocklistIndividualSatellites:
 *
 * 1) Turns on location, waits for 3 locations, ensuring they are valid, and checks corresponding
 * GnssStatus for common satellites (strongest and one other.)
 * 2a & b) Turns off location, and blocklists common satellites.
 * 3) Restart location, wait for 3 locations, ensuring they are valid, and checks corresponding
 * GnssStatus does not use those satellites.
 * 4a & b) Turns off location, and send in empty blocklist.
 * 5a) Restart location, wait for 3 locations, ensuring they are valid, and checks corresponding
 * GnssStatus does re-use at least the previously strongest satellite
 * 5b) Retry a few times, in case GNSS search strategy takes a while to reacquire even the
 * formerly strongest satellite
 */
TEST_P(GnssHalTest, BlocklistIndividualSatellites) {
    if (!(aidl_gnss_cb_->last_capabilities_ &
          (int)GnssCallbackAidl::CAPABILITY_SATELLITE_BLOCKLIST)) {
        ALOGI("Test BlocklistIndividualSatellites skipped. SATELLITE_BLOCKLIST capability not "
              "supported.");
        return;
    }

    const int kLocationsToAwait = 3;
    const int kRetriesToUnBlocklist = 10;

    gnss_cb_->location_cbq_.reset();
    StartAndCheckLocations(kLocationsToAwait);
    int location_called_count = gnss_cb_->location_cbq_.calledCount();

    // Tolerate 1 less sv status to handle edge cases in reporting.
    int sv_info_list_cbq_size = gnss_cb_->sv_info_list_cbq_.size();
    EXPECT_GE(sv_info_list_cbq_size + 1, kLocationsToAwait);
    ALOGD("Observed %d GnssSvInfo, while awaiting %d Locations (%d received)",
          sv_info_list_cbq_size, kLocationsToAwait, location_called_count);

    /*
     * Identify strongest SV seen at least kLocationsToAwait -1 times
     * Why -1?  To avoid test flakiness in case of (plausible) slight flakiness in strongest signal
     * observability (one epoch RF null)
     */

    const int kGnssSvInfoListTimeout = 2;
    std::list<hidl_vec<IGnssCallback_2_1::GnssSvInfo>> sv_info_vec_list;
    int count = gnss_cb_->sv_info_list_cbq_.retrieve(sv_info_vec_list, sv_info_list_cbq_size,
                                                     kGnssSvInfoListTimeout);

    ASSERT_EQ(count, sv_info_list_cbq_size);

    BlocklistedSource source_to_blocklist =
            FindStrongFrequentNonGpsSource(sv_info_vec_list, kLocationsToAwait - 1);

    if (source_to_blocklist.constellation == GnssConstellationTypeAidl::UNKNOWN) {
        // Cannot find a non-GPS satellite. Let the test pass.
        ALOGD("Cannot find a non-GPS satellite. Letting the test pass.");
        return;
    }

    // Stop locations, blocklist the common SV
    StopAndClearLocations();

    sp<IGnssConfiguration> gnss_configuration_hal;
    auto status = aidl_gnss_hal_->getExtensionGnssConfiguration(&gnss_configuration_hal);
    ASSERT_TRUE(status.isOk());
    ASSERT_NE(gnss_configuration_hal, nullptr);

    std::vector<BlocklistedSource> sources;
    sources.resize(1);
    sources[0] = source_to_blocklist;

    status = gnss_configuration_hal->setBlocklist(sources);
    ASSERT_TRUE(status.isOk());

    // retry and ensure satellite not used
    gnss_cb_->sv_info_list_cbq_.reset();

    gnss_cb_->location_cbq_.reset();
    StartAndCheckLocations(kLocationsToAwait);

    // early exit if test is being run with insufficient signal
    location_called_count = gnss_cb_->location_cbq_.calledCount();
    if (location_called_count == 0) {
        ALOGE("0 Gnss locations received - ensure sufficient signal and retry");
    }
    ASSERT_TRUE(location_called_count > 0);

    // Tolerate 1 less sv status to handle edge cases in reporting.
    sv_info_list_cbq_size = gnss_cb_->sv_info_list_cbq_.size();
    EXPECT_GE(sv_info_list_cbq_size + 1, kLocationsToAwait);
    ALOGD("Observed %d GnssSvInfo, while awaiting %d Locations (%d received)",
          sv_info_list_cbq_size, kLocationsToAwait, location_called_count);
    for (int i = 0; i < sv_info_list_cbq_size; ++i) {
        hidl_vec<IGnssCallback_2_1::GnssSvInfo> sv_info_vec;
        gnss_cb_->sv_info_list_cbq_.retrieve(sv_info_vec, kGnssSvInfoListTimeout);
        for (uint32_t iSv = 0; iSv < sv_info_vec.size(); iSv++) {
            const auto& gnss_sv = sv_info_vec[iSv];
            EXPECT_FALSE((gnss_sv.v2_0.v1_0.svid == source_to_blocklist.svid) &&
                         (static_cast<GnssConstellationTypeAidl>(gnss_sv.v2_0.constellation) ==
                          source_to_blocklist.constellation) &&
                         (gnss_sv.v2_0.v1_0.svFlag & IGnssCallback_1_0::GnssSvFlags::USED_IN_FIX));
        }
    }

    // clear blocklist and restart - this time updating the blocklist while location is still on
    sources.resize(0);

    status = gnss_configuration_hal->setBlocklist(sources);
    ASSERT_TRUE(status.isOk());

    bool strongest_sv_is_reobserved = false;
    // do several loops awaiting a few locations, allowing non-immediate reacquisition strategies
    int unblocklist_loops_remaining = kRetriesToUnBlocklist;
    while (!strongest_sv_is_reobserved && (unblocklist_loops_remaining-- > 0)) {
        StopAndClearLocations();
        gnss_cb_->sv_info_list_cbq_.reset();

        gnss_cb_->location_cbq_.reset();
        StartAndCheckLocations(kLocationsToAwait);

        // early exit loop if test is being run with insufficient signal
        location_called_count = gnss_cb_->location_cbq_.calledCount();
        if (location_called_count == 0) {
            ALOGE("0 Gnss locations received - ensure sufficient signal and retry");
        }
        ASSERT_TRUE(location_called_count > 0);

        // Tolerate 1 less sv status to handle edge cases in reporting.
        sv_info_list_cbq_size = gnss_cb_->sv_info_list_cbq_.size();
        EXPECT_GE(sv_info_list_cbq_size + 1, kLocationsToAwait);
        ALOGD("Clear blocklist, observed %d GnssSvInfo, while awaiting %d Locations"
              ", tries remaining %d",
              sv_info_list_cbq_size, kLocationsToAwait, unblocklist_loops_remaining);

        for (int i = 0; i < sv_info_list_cbq_size; ++i) {
            hidl_vec<IGnssCallback_2_1::GnssSvInfo> sv_info_vec;
            gnss_cb_->sv_info_list_cbq_.retrieve(sv_info_vec, kGnssSvInfoListTimeout);
            for (uint32_t iSv = 0; iSv < sv_info_vec.size(); iSv++) {
                const auto& gnss_sv = sv_info_vec[iSv];
                if ((gnss_sv.v2_0.v1_0.svid == source_to_blocklist.svid) &&
                    (static_cast<GnssConstellationTypeAidl>(gnss_sv.v2_0.constellation) ==
                     source_to_blocklist.constellation) &&
                    (gnss_sv.v2_0.v1_0.svFlag & IGnssCallback_1_0::GnssSvFlags::USED_IN_FIX)) {
                    strongest_sv_is_reobserved = true;
                    break;
                }
            }
            if (strongest_sv_is_reobserved) break;
        }
    }
    EXPECT_TRUE(strongest_sv_is_reobserved);
    StopAndClearLocations();
}

/*
 * BlocklistConstellationLocationOff:
 *
 * 1) Turns on location, waits for 3 locations, ensuring they are valid, and checks corresponding
 * GnssStatus for any non-GPS constellations.
 * 2a & b) Turns off location, and blocklist first non-GPS constellations.
 * 3) Restart location, wait for 3 locations, ensuring they are valid, and checks corresponding
 * GnssStatus does not use any constellation but GPS.
 * 4a & b) Clean up by turning off location, and send in empty blocklist.
 */
TEST_P(GnssHalTest, BlocklistConstellationLocationOff) {
    if (!(aidl_gnss_cb_->last_capabilities_ &
          (int)GnssCallbackAidl::CAPABILITY_SATELLITE_BLOCKLIST)) {
        ALOGI("Test BlocklistConstellationLocationOff skipped. SATELLITE_BLOCKLIST capability not "
              "supported.");
        return;
    }

    const int kLocationsToAwait = 3;
    const int kGnssSvInfoListTimeout = 2;

    // Find first non-GPS constellation to blocklist
    GnssConstellationTypeAidl constellation_to_blocklist = static_cast<GnssConstellationTypeAidl>(
            startLocationAndGetNonGpsConstellation(kLocationsToAwait, kGnssSvInfoListTimeout));

    // Turns off location
    StopAndClearLocations();

    BlocklistedSource source_to_blocklist_1;
    source_to_blocklist_1.constellation = constellation_to_blocklist;
    source_to_blocklist_1.svid = 0;  // documented wildcard for all satellites in this constellation

    // IRNSS was added in 2.0. Always attempt to blocklist IRNSS to verify that the new enum is
    // supported.
    BlocklistedSource source_to_blocklist_2;
    source_to_blocklist_2.constellation = GnssConstellationTypeAidl::IRNSS;
    source_to_blocklist_2.svid = 0;  // documented wildcard for all satellites in this constellation

    sp<IGnssConfiguration> gnss_configuration_hal;
    auto status = aidl_gnss_hal_->getExtensionGnssConfiguration(&gnss_configuration_hal);
    ASSERT_TRUE(status.isOk());
    ASSERT_NE(gnss_configuration_hal, nullptr);

    hidl_vec<BlocklistedSource> sources;
    sources.resize(2);
    sources[0] = source_to_blocklist_1;
    sources[1] = source_to_blocklist_2;

    status = gnss_configuration_hal->setBlocklist(sources);
    ASSERT_TRUE(status.isOk());

    // retry and ensure constellation not used
    gnss_cb_->sv_info_list_cbq_.reset();

    gnss_cb_->location_cbq_.reset();
    StartAndCheckLocations(kLocationsToAwait);

    // Tolerate 1 less sv status to handle edge cases in reporting.
    int sv_info_list_cbq_size = gnss_cb_->sv_info_list_cbq_.size();
    EXPECT_GE(sv_info_list_cbq_size + 1, kLocationsToAwait);
    ALOGD("Observed %d GnssSvInfo, while awaiting %d Locations", sv_info_list_cbq_size,
          kLocationsToAwait);
    for (int i = 0; i < sv_info_list_cbq_size; ++i) {
        hidl_vec<IGnssCallback_2_1::GnssSvInfo> sv_info_vec;
        gnss_cb_->sv_info_list_cbq_.retrieve(sv_info_vec, kGnssSvInfoListTimeout);
        for (uint32_t iSv = 0; iSv < sv_info_vec.size(); iSv++) {
            const auto& gnss_sv = sv_info_vec[iSv];
            EXPECT_FALSE((static_cast<GnssConstellationTypeAidl>(gnss_sv.v2_0.constellation) ==
                          source_to_blocklist_1.constellation) &&
                         (gnss_sv.v2_0.v1_0.svFlag & IGnssCallback_1_0::GnssSvFlags::USED_IN_FIX));
            EXPECT_FALSE((static_cast<GnssConstellationTypeAidl>(gnss_sv.v2_0.constellation) ==
                          source_to_blocklist_2.constellation) &&
                         (gnss_sv.v2_0.v1_0.svFlag & IGnssCallback_1_0::GnssSvFlags::USED_IN_FIX));
        }
    }

    // clean up
    StopAndClearLocations();
    sources.resize(0);
    status = gnss_configuration_hal->setBlocklist(sources);
    ASSERT_TRUE(status.isOk());
}

/*
 * BlocklistConstellationLocationOn:
 *
 * 1) Turns on location, waits for 3 locations, ensuring they are valid, and checks corresponding
 * GnssStatus for any non-GPS constellations.
 * 2a & b) Blocklist first non-GPS constellation, and turn off location.
 * 3) Restart location, wait for 3 locations, ensuring they are valid, and checks corresponding
 * GnssStatus does not use any constellation but GPS.
 * 4a & b) Clean up by turning off location, and send in empty blocklist.
 */
TEST_P(GnssHalTest, BlocklistConstellationLocationOn) {
    if (!(aidl_gnss_cb_->last_capabilities_ &
          (int)GnssCallbackAidl::CAPABILITY_SATELLITE_BLOCKLIST)) {
        ALOGI("Test BlocklistConstellationLocationOn skipped. SATELLITE_BLOCKLIST capability not "
              "supported.");
        return;
    }

    const int kLocationsToAwait = 3;
    const int kGnssSvInfoListTimeout = 2;

    // Find first non-GPS constellation to blocklist
    GnssConstellationTypeAidl constellation_to_blocklist = static_cast<GnssConstellationTypeAidl>(
            startLocationAndGetNonGpsConstellation(kLocationsToAwait, kGnssSvInfoListTimeout));

    BlocklistedSource source_to_blocklist_1;
    source_to_blocklist_1.constellation = constellation_to_blocklist;
    source_to_blocklist_1.svid = 0;  // documented wildcard for all satellites in this constellation

    // IRNSS was added in 2.0. Always attempt to blocklist IRNSS to verify that the new enum is
    // supported.
    BlocklistedSource source_to_blocklist_2;
    source_to_blocklist_2.constellation = GnssConstellationTypeAidl::IRNSS;
    source_to_blocklist_2.svid = 0;  // documented wildcard for all satellites in this constellation

    sp<IGnssConfiguration> gnss_configuration_hal;
    auto status = aidl_gnss_hal_->getExtensionGnssConfiguration(&gnss_configuration_hal);
    ASSERT_TRUE(status.isOk());
    ASSERT_NE(gnss_configuration_hal, nullptr);

    hidl_vec<BlocklistedSource> sources;
    sources.resize(2);
    sources[0] = source_to_blocklist_1;
    sources[1] = source_to_blocklist_2;

    status = gnss_configuration_hal->setBlocklist(sources);
    ASSERT_TRUE(status.isOk());

    // Turns off location
    StopAndClearLocations();

    // retry and ensure constellation not used
    gnss_cb_->sv_info_list_cbq_.reset();

    gnss_cb_->location_cbq_.reset();
    StartAndCheckLocations(kLocationsToAwait);

    // Tolerate 1 less sv status to handle edge cases in reporting.
    int sv_info_list_cbq_size = gnss_cb_->sv_info_list_cbq_.size();
    EXPECT_GE(sv_info_list_cbq_size + 1, kLocationsToAwait);
    ALOGD("Observed %d GnssSvInfo, while awaiting %d Locations", sv_info_list_cbq_size,
          kLocationsToAwait);
    for (int i = 0; i < sv_info_list_cbq_size; ++i) {
        hidl_vec<IGnssCallback_2_1::GnssSvInfo> sv_info_vec;
        gnss_cb_->sv_info_list_cbq_.retrieve(sv_info_vec, kGnssSvInfoListTimeout);
        for (uint32_t iSv = 0; iSv < sv_info_vec.size(); iSv++) {
            const auto& gnss_sv = sv_info_vec[iSv];
            EXPECT_FALSE((static_cast<GnssConstellationTypeAidl>(gnss_sv.v2_0.constellation) ==
                          source_to_blocklist_1.constellation) &&
                         (gnss_sv.v2_0.v1_0.svFlag & IGnssCallback_1_0::GnssSvFlags::USED_IN_FIX));
            EXPECT_FALSE((static_cast<GnssConstellationTypeAidl>(gnss_sv.v2_0.constellation) ==
                          source_to_blocklist_2.constellation) &&
                         (gnss_sv.v2_0.v1_0.svFlag & IGnssCallback_1_0::GnssSvFlags::USED_IN_FIX));
        }
    }

    // clean up
    StopAndClearLocations();
    sources.resize(0);
    status = gnss_configuration_hal->setBlocklist(sources);
    ASSERT_TRUE(status.isOk());
}