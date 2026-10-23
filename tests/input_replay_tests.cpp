#include "jfg/runtime/input_replay.hpp"
#include "jfg/runtime/modern_controls.hpp"
#include "jfg/runtime/modern_roll.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

class TemporaryReplay final {
public:
    explicit TemporaryReplay(const std::string_view contents) {
        static std::atomic<std::uint64_t> sequence{0U};
        const auto nonce =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("jfg-input-replay-" + std::to_string(nonce) + "-" +
             std::to_string(sequence.fetch_add(1U)) + ".txt");
        std::ofstream stream(path_, std::ios::binary);
        stream.write(contents.data(),
                     static_cast<std::streamsize>(contents.size()));
        if (!stream) {
            throw std::runtime_error("failed to write input replay fixture");
        }
    }

    ~TemporaryReplay() noexcept {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void test_valid_replay() {
    const TemporaryReplay fixture(
        "jfg-phase8-input-v1\r\n"
        "1800,1810,1000,0,0\r\n"
        "2050,2060,8000,-12,34\r\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(fixture.path(), replay) ==
              jfg::InputReplayError::none,
          "valid replay should load");
    check(replay.events().size() == 2U, "valid replay event count");
    check(replay.sample_at(1799U).neutral(), "pre-event sample is neutral");
    check(replay.sample_at(1800U).buttons == 0x1000U,
          "first boundary is inclusive");
    check(replay.sample_at(1809U).buttons == 0x1000U,
          "event remains active before end");
    check(replay.sample_at(1810U).neutral(), "end boundary is exclusive");
    check(replay.sample_at(2055U) ==
              jfg::ControllerReplaySample{true, 0x8000U, -12, 34},
          "later event preserves buttons and stick values");
}

void test_connection_transitions() {
    const TemporaryReplay fixture(
        "jfg-phase8-input-v2\n"
        "100,110,0,0000,0,0\n"
        "120,130,1,8000,12,-34\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(fixture.path(), replay) ==
              jfg::InputReplayError::none,
          "connection replay should load");
    check(!replay.sample_at(105U).connected,
          "disconnect window is observable");
    check(replay.sample_at(110U).connected,
          "controller reconnects after disconnect window");
    check(replay.sample_at(125U) ==
              jfg::ControllerReplaySample{true, 0x8000U, 12, -34},
          "connected v2 sample preserves controls");
}

void test_poll_replay_eof_is_neutral() {
    const TemporaryReplay fixture(
        "jfg-phase8-input-v2\n"
        "0,1,1,0000,0,0\n"
        "1,100,1,8000,12,-34\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(fixture.path(), replay) ==
              jfg::InputReplayError::none,
          "poll replay should load");
    check(replay.sample_by_poll(0U).neutral(), "first poll is neutral");
    check(replay.sample_by_poll(1U) ==
              jfg::ControllerReplaySample{true, 0x8000U, 12, -34},
          "last recorded poll preserves buttons and stick");
    check(replay.sample_by_poll(2U).neutral(),
          "first poll after EOF is neutral despite long final interval");
    check(replay.sample_by_poll(100U).neutral(),
          "later polls after EOF remain neutral");
}

void test_long_manual_recording() {
    std::string contents = "jfg-phase8-input-v2\n";
    for (std::size_t i = 0U; i < jfg::kMaximumInputReplayEvents; ++i) {
        contents += std::to_string(i * 2U) + "," +
                    std::to_string(i * 2U + 1U) + ",1,8000,12,-34\n";
    }
    check(contents.size() > 1024U * 1024U,
          "manual recording exceeds the former one MiB limit");
    jfg::DeterministicInputReplay replay;
    const TemporaryReplay valid(contents);
    check(jfg::DeterministicInputReplay::load(valid.path(), replay) ==
              jfg::InputReplayError::none, "long manual recording loads");
    check(replay.sample_by_poll(jfg::kMaximumInputReplayEvents - 1U) ==
              jfg::ControllerReplaySample{true, 0x8000U, 12, -34},
          "final input of long recording remains intact");
    contents += "900000,900001,1,0,0,0\n";
    const TemporaryReplay excess(contents);
    check(jfg::DeterministicInputReplay::load(excess.path(), replay) ==
              jfg::InputReplayError::invalid_record,
          "event limit still rejects oversized event counts");
    const TemporaryReplay too_large(std::string(8U * 1024U * 1024U + 1U, 'x'));
    check(jfg::DeterministicInputReplay::load(too_large.path(), replay) ==
              jfg::InputReplayError::too_large, "byte limit remains enforced");
}

void test_independent_look() {
    const TemporaryReplay valid("jfg-phase8-input-v3\n100,120,1,2010,80,-40,3,180,-90\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(valid.path(),replay)==jfg::InputReplayError::none,"v3 independent look loads");
    const auto sample=replay.sample_at(105);
    check(sample.stick_x==80 && sample.stick_y==-40 && sample.look_x==180 && sample.look_y==-90 && sample.pc_mode==3,"movement and mouse look stay independent");
    check(replay.sample_at(120).pc_mode==0 && replay.sample_at(120).neutral(),"look ends at event boundary");
    const auto before=replay.events();
    for(const auto* row:{"1,2,1,0,0,0,5,0,0","1,2,1,0,0,0,0,1,0",
        "1,2,0,0,0,0,3,1,0","1,2,1,0,0,0,2,128,0","1,2,1,0,0,0,3,32768,0",
        "1,2,1,0,0,0,3,2","1,2,1,0,0,0,3,1,2,3"}) {
      const TemporaryReplay bad(std::string("jfg-phase8-input-v3\n")+row+"\n");
      check(jfg::DeterministicInputReplay::load(bad.path(),replay)==jfg::InputReplayError::invalid_record && replay.events()==before,"invalid v3 leaves active replay intact");
    }
}


void test_modern_action_recording() {
    const TemporaryReplay valid("jfg-phase8-input-v5\n100,120,1,2010,80,-40,5,6144,-18432,129\n120,121,1,8000,0,0,0,0,0,128\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(valid.path(),replay)==jfg::InputReplayError::none,"v5 modern actions load");
    check(replay.sample_at(110).modern_flags==129 && replay.sample_at(110).look_y==-18432,"v5 scope commands and precise look survive");
    check(replay.sample_at(120).modern_flags==128 && replay.sample_at(120).buttons==0x8000,"modern menus do not require captured mouse look");
    check(replay.sample_at(121).neutral(),"modern actions release at EOF");
    const auto before=replay.events();
    for(const auto *row:{"1,2,1,0,0,0,0,0,0,1","1,2,1,0,0,0,0,0,0,160",
        "1,2,0,0,0,0,0,0,0,128","1,2,1,0,0,0,0,0,0,256",
        "1,2,1,0,0,0,0,0,0","1,2,1,0,0,0,0,0,0,128,0"}) {
        const TemporaryReplay bad(std::string("jfg-phase8-input-v5\n")+row+"\n");
        check(jfg::DeterministicInputReplay::load(bad.path(),replay)==jfg::InputReplayError::invalid_record && replay.events()==before,"invalid modern flags reject atomically");
    }
    const TemporaryReplay old("jfg-phase8-input-v4\n1,2,1,0,0,0,0,0,0,128\n");
    check(jfg::DeterministicInputReplay::load(old.path(),replay)==jfg::InputReplayError::invalid_record,"v4 does not accept action semantics");
    const TemporaryReplay compatible("jfg-phase8-input-v5\n1,2,1,0,0,0,5,80,0,0\n");
    check(jfg::DeterministicInputReplay::load(compatible.path(),replay)==jfg::InputReplayError::none && replay.sample_at(1).modern_flags==0,"v5 retains legacy mapping semantics when disabled");
}

void test_modern_roll_recording() {
    const TemporaryReplay valid("jfg-phase8-input-v8\n"
        "1,2,1,0,-80,0,3,12,-9,136,0,0\n"
        "2,3,1,0,80,0,5,-12,9,144,0,0\n"
        "3,4,1,0,-80,0,3,0,0,128,0,0\n"
        "4,5,1,0,-80,0,3,0,0,143,0,0\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(valid.path(),replay)==jfg::InputReplayError::none,
        "explicit roll intent loads without a format rewrite");
    check(jfg::modern_roll_side_buttons(replay.sample_by_poll(0).modern_flags)==2 &&
        jfg::modern_roll_side_buttons(replay.sample_by_poll(1).modern_flags)==1,
        "keyboard and controller roll directions survive recording");
    check(jfg::modern_roll_side_buttons(replay.sample_by_poll(2).modern_flags)==0 &&
        replay.sample_by_poll(2).stick_x==-80,"old/no-intent frames never derive a roll from lateral axes");
    check((replay.sample_by_poll(3).modern_flags&7U)==7U &&
        jfg::modern_roll_side_buttons(replay.sample_by_poll(3).modern_flags)==2,"scope flags coexist with roll intent");
    const auto before=replay.events();
    std::ostringstream serialized;serialized<<jfg::kInputReplayHeaderV8<<'\n';
    for(const auto &event:before) {
        const auto &sample=event.sample;
        serialized<<event.first_retrace<<','<<event.end_retrace<<','<<int(sample.connected)<<','
            <<std::hex<<sample.buttons<<std::dec<<','<<int(sample.stick_x)<<','<<int(sample.stick_y)<<','
            <<int(sample.pc_mode)<<','<<sample.look_x<<','<<sample.look_y<<','<<int(sample.modern_flags)<<','
            <<int(sample.menu_kind)<<','<<int(sample.menu_target)<<'\n';
    }
    const TemporaryReplay roundtrip(serialized.str());
    check(jfg::DeterministicInputReplay::load(roundtrip.path(),replay)==jfg::InputReplayError::none &&
        replay.events()==before,"current packet serialization roundtrips roll intent and other fields");
    for(const auto *row:{"1,2,1,0,0,0,3,0,0,8,0,0","1,2,1,0,0,0,5,0,0,16,0,0",
        "1,2,1,0,0,0,3,0,0,160,0,0","1,2,1,0,0,0,3,0,0,192,0,0",
        "1,2,0,0,0,0,0,0,0,136,0,0","1,2,1,0,0,0,3,0,0,384,0,0"}) {
        const TemporaryReplay bad(std::string("jfg-phase8-input-v8\n")+row+"\n");
        check(jfg::DeterministicInputReplay::load(bad.path(),replay)==jfg::InputReplayError::invalid_record &&
            replay.events()==before,"roll flags still require Modern identity, connection and known bits");
    }
    const TemporaryReplay legacy("jfg-phase8-input-v5\n1,2,1,0,-80,0,3,0,0,128\n");
    check(jfg::DeterministicInputReplay::load(legacy.path(),replay)==jfg::InputReplayError::none &&
        jfg::modern_roll_side_buttons(replay.sample_at(1).modern_flags)==0,"legacy Modern replay behavior remains unchanged");
}

void test_native_menu_recording() {

  {
    const TemporaryReplay valid("jfg-phase8-input-v8\n1,2,1,8000,0,0,0,0,0,128,8,8\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(valid.path(),replay)==jfg::InputReplayError::none && replay.sample_at(1).menu_kind==8,"v8 Options direct selection survives recording");
    for(const auto *row:{"1,2,1,0,0,0,0,0,0,128,8,9","1,2,1,0,0,0,0,0,0,128,6,1","1,2,1,0,0,0,0,0,0,128,7,1","1,2,1,0,0,0,0,0,0,128,9,1"}) {
      const TemporaryReplay bad(std::string("jfg-phase8-input-v8\n")+row+"\n");
      check(jfg::DeterministicInputReplay::load(bad.path(),replay)==jfg::InputReplayError::invalid_record,"nonselection pages and invalid Options targets reject");
    }
    const TemporaryReplay old("jfg-phase8-input-v7\n1,2,1,8000,0,0,0,0,0,128,8,1\n");
    check(jfg::DeterministicInputReplay::load(old.path(),replay)==jfg::InputReplayError::invalid_record,"v7 does not reinterpret Options events");
  }
  {
    const TemporaryReplay validPause("jfg-phase8-input-v7\n100,101,1,0,0,0,0,0,0,128,5,7\n101,102,1,2000,0,0,0,0,0,128,5,9\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(validPause.path(),replay)==jfg::InputReplayError::none,"v7 pause events load");
    check(replay.sample_at(100).menu_target==7&&replay.sample_at(101).buttons==0x2000,"pause hover and native Save survive recording");
    const TemporaryReplay invalidPause("jfg-phase8-input-v7\n100,101,1,2000,0,0,0,0,0,128,5,10\n");
    check(jfg::DeterministicInputReplay::load(invalidPause.path(),replay)==jfg::InputReplayError::invalid_record,"invalid pause target rejected");
  }
    const TemporaryReplay valid("jfg-phase8-input-v6\n100,101,1,0,0,0,0,0,0,128,3,5\n101,102,1,8000,0,0,0,0,0,128,3,5\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(valid.path(),replay)==jfg::InputReplayError::none,"v6 direct menu selection loads");
    const auto hover=replay.sample_at(100);
    check(hover.menu_kind==3&&hover.menu_target==5&&!hover.buttons&&!hover.stick_x&&!hover.stick_y&&!hover.neutral(),"hover carries slot five without controller movement");
    check(replay.sample_at(101).buttons==0x8000&&replay.sample_at(101).menu_target==5,"click records selection and activation together");
    check(replay.sample_at(102).neutral(),"semantic event releases at EOF");
    const auto before=replay.events();
    for(const auto *row:{"1,2,1,0,0,0,0,0,0,0,3,7","1,2,1,0,0,0,0,0,0,0,2,4",
        "1,2,1,0,0,0,0,0,0,0,0,1","1,2,1,0,0,0,0,0,0,0,3,0",
        "1,2,1,0,0,0,0,0,0,0,1,1","1,2,1,0,0,0,0,0,0,0,4,2",
        "1,2,0,0,0,0,0,0,0,0,3,1","1,2,1,0,0,0,0,0,0,0,3"}) {
        const TemporaryReplay bad(std::string("jfg-phase8-input-v6\n")+row+"\n");
        check(jfg::DeterministicInputReplay::load(bad.path(),replay)==jfg::InputReplayError::invalid_record&&replay.events()==before,"invalid menu semantics reject atomically");
    }
}

void test_precise_stick_recording() {
    const TemporaryReplay valid("jfg-phase8-input-v4\n100,120,1,2010,80,-40,5,6144,-18432\n120,121,1,0,0,0,3,32000,0\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(valid.path(),replay)==jfg::InputReplayError::none,"v4 precise stick and mouse load");
    const auto sample=replay.sample_at(110);
    check(sample.pc_mode==5&&sample.look_x==6144&&sample.look_y==-18432&&sample.stick_x==80,"v4 preserves modern rate independently from movement");
    check(replay.sample_at(121).neutral(),"v4 look releases at EOF");
    const auto before=replay.events();
    for(const auto* row:{"1,2,1,0,0,0,5,18433,0","1,2,1,0,0,0,5,0,-18433",
        "1,2,0,0,0,0,5,1,0","1,2,1,0,0,0,6,0,0","1,2,1,0,0,0,4,128,0"}) {
      const TemporaryReplay bad(std::string("jfg-phase8-input-v4\n")+row+"\n");
      check(jfg::DeterministicInputReplay::load(bad.path(),replay)==jfg::InputReplayError::invalid_record&&replay.events()==before,"invalid v4 is rejected atomically");
    }
    const TemporaryReplay old("jfg-phase8-input-v3\n1,2,1,0,0,0,5,1,0\n");
    check(jfg::DeterministicInputReplay::load(old.path(),replay)==jfg::InputReplayError::invalid_record,"v3 never reinterprets precise rate mode");
    const TemporaryReplay compatible("jfg-phase8-input-v4\n1,2,1,0,0,0,4,80,0\n");
    check(jfg::DeterministicInputReplay::load(compatible.path(),replay)==jfg::InputReplayError::none&&replay.sample_at(1).look_x==80,"legacy mode keeps units in a v4 recording");
}

void test_repeated_poll_records() {
    const TemporaryReplay fixture(
        "jfg-phase8-input-v3\n"
        "21,22,1,8000,0,0,3,12,0\n"
        "21,22,1,0000,0,0,3,-7,0\n"
        "22,23,1,0010,0,0,0,0,0\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(fixture.path(), replay) ==
        jfg::InputReplayError::invalid_order, "retrace replay still rejects ambiguous repeated intervals");
    check(jfg::DeterministicInputReplay::load(fixture.path(), replay,
        jfg::InputReplayOrder::recorded_poll) == jfg::InputReplayError::none,
        "original OS may poll twice in one retrace");
    check(replay.events().size() == 3 && replay.sample_by_poll(0).buttons == 0x8000 &&
        replay.sample_by_poll(0).look_x == 12 && replay.sample_by_poll(1).look_x == -7 &&
        replay.sample_by_poll(2).buttons == 0x0010 && replay.sample_by_poll(3).neutral(),
        "recorded poll order preserves independent transitions and EOF");
    const auto before = replay.events();
    for (const auto* rows : {
        "21,23,1,0,0,0\n22,23,1,0,0,0\n",
        "21,22,1,0,0,0\n20,21,1,0,0,0\n",
        "21,23,1,0,0,0\n21,23,1,0,0,0\n"}) {
        const TemporaryReplay invalid(std::string("jfg-phase8-input-v2\n")+rows);
        check(jfg::DeterministicInputReplay::load(invalid.path(), replay,
            jfg::InputReplayOrder::recorded_poll) == jfg::InputReplayError::invalid_order &&
            replay.events() == before, "poll mode rejects reordered or overlapping intervals atomically");
    }
}

void test_rejects_invalid_records() {
    for (const std::string_view contents : {
             "wrong-header\n1800,1810,1000,0,0\n",
             "jfg-phase8-input-v1\n1800,1800,1000,0,0\n",
             "jfg-phase8-input-v1\n1800,1810,10000,0,0\n",
             "jfg-phase8-input-v1\n1800,1810,1000,-129,0\n",
             "jfg-phase8-input-v1\n1800,1810,1000,0,0\n1809,1820,0,0,0\n",
             "jfg-phase8-input-v2\n1800,1810,2,0000,0,0\n",
             "jfg-phase8-input-v2\n1800,1810,0,8000,0,0\n",
         }) {
        const TemporaryReplay fixture(contents);
        jfg::DeterministicInputReplay replay;
        check(jfg::DeterministicInputReplay::load(fixture.path(), replay) !=
                  jfg::InputReplayError::none,
              "invalid replay should fail closed");
    }
}

void test_replay_modern_controls_follow_recording() {
    const TemporaryReplay recording("jfg-phase8-input-v8\n"
        "1,2,1,0,0,0,0,0,0,0,0,0\n"
        "2,3,1,0,0,0,3,0,0,128,0,0\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(recording.path(),replay)==jfg::InputReplayError::none,
        "Modern replay recording loads");
    jfg::ReplayModernControls modern;
    check(!modern.active(),"replayed Modern Controls start off");
    check(!modern.observe(replay.sample_at(1)) && !modern.active(),"a neutral Controller Mapping sample keeps Modern off");
    check(modern.observe(replay.sample_at(2)) && modern.active(),"a recorded Modern sample turns Modern on");
}

void test_replay_modern_controls_hold_until_mapping_input() {
    // Device-release waits record neutral flag-0 samples; a disconnect records none.
    const TemporaryReplay recording("jfg-phase8-input-v8\n"
        "1,2,1,0,0,0,3,0,0,128,0,0\n"
        "2,3,1,0,0,0,0,0,0,0,0,0\n"
        "3,4,0,0,0,0,0,0,0,0,0,0\n"
        "4,5,1,0,0,0,3,0,0,128,0,0\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(recording.path(),replay)==jfg::InputReplayError::none,
        "Modern recording with release wait and disconnect loads");
    jfg::ReplayModernControls modern;
    (void)modern.observe(replay.sample_at(1));
    check(!modern.observe(replay.sample_at(2)) && modern.active(),"a neutral flag-0 sample keeps Modern on");
    check(!modern.observe(replay.sample_at(3)) && modern.active(),"a disconnected sample keeps Modern on");

    const TemporaryReplay mapping_input("jfg-phase8-input-v8\n"
        "1,2,1,8000,0,0,0,0,0,0,0,0\n"
        "2,3,1,0,0,12,0,0,0,0,0,0\n"
        "3,4,1,0,0,0,3,0,-5,0,0,0\n"
        "4,5,1,0,0,0,1,0,0,0,0,0\n"
        "5,6,1,0,0,0,2,0,0,0,0,0\n");
    check(jfg::DeterministicInputReplay::load(mapping_input.path(),replay)==jfg::InputReplayError::none,
        "Controller Mapping input samples load");
    const char* const descriptions[]{"buttons","stick","look","legacy aim mode 1","legacy aim mode 2"};
    for(std::uint64_t retrace=1;retrace<=5;++retrace) {
        jfg::ReplayModernControls switched;
        (void)switched.observe(jfg::ControllerReplaySample{true,0,0,0,3,0,0,jfg::kModernActionsFlag});
        check(switched.observe(replay.sample_at(retrace)) && !switched.active(),
            std::string("Controller Mapping ")+descriptions[retrace-1]+" turns Modern off");
    }
}

std::string replay_modern_trace(const jfg::DeterministicInputReplay& replay) {
    jfg::ReplayModernControls modern;
    std::string trace;
    for(std::uint64_t poll=0;poll<replay.events().size();++poll) {
        (void)modern.observe(replay.sample_by_poll(poll));
        trace+=modern.active()?'M':'C';
    }
    return trace;
}

void test_replay_modern_controls_switch_mid_recording() {
    const TemporaryReplay recording("jfg-phase8-input-v8\n"
        "1,2,1,8000,0,0,0,0,0,0,0,0\n"
        "2,3,1,0,0,0,0,0,0,0,0,0\n"
        "3,4,1,0,40,0,3,0,0,128,0,0\n"
        "4,5,1,0,0,0,0,0,0,0,0,0\n"
        "5,6,1,0,0,30,0,0,0,0,0,0\n"
        "6,7,1,0,0,0,0,0,0,0,0,0\n"
        "7,8,1,0,0,0,3,0,0,128,0,0\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(recording.path(),replay,jfg::InputReplayOrder::recorded_poll)==
        jfg::InputReplayError::none,"mixed-style recording loads");
    check(replay_modern_trace(replay)=="CCMMCCM","style switches mid-recording follow the recorded samples both ways");
}

void test_replay_modern_controls_old_formats_stay_off() {
    for(const std::string_view contents:{
        std::string_view("jfg-phase8-input-v1\n1,2,1000,12,-34\n"),
        std::string_view("jfg-phase8-input-v2\n1,2,1,8000,12,-34\n2,3,1,0,0,0\n"),
        std::string_view("jfg-phase8-input-v3\n1,2,1,0,0,0,3,12,0\n2,3,1,0010,0,0,1,0,0\n"),
        std::string_view("jfg-phase8-input-v4\n1,2,1,2010,80,-40,5,6144,-18432\n2,3,1,0,0,0,0,0,0\n")}) {
        const TemporaryReplay recording(contents);
        jfg::DeterministicInputReplay replay;
        check(jfg::DeterministicInputReplay::load(recording.path(),replay,jfg::InputReplayOrder::recorded_poll)==
            jfg::InputReplayError::none,"pre-Modern recording loads");
        check(replay_modern_trace(replay).find('M')==std::string::npos,"v1-v4 recordings never turn Modern on");
    }
}

std::string serialize_v9(const std::vector<jfg::InputReplayEvent>& events) {
    std::ostringstream serialized;serialized<<jfg::kInputReplayHeaderV9<<'\n';
    for(const auto &event:events) {
        const auto &sample=event.sample;
        serialized<<event.first_retrace<<','<<event.end_retrace<<','<<int(sample.connected)<<','
            <<std::hex<<sample.buttons<<std::dec<<','<<int(sample.stick_x)<<','<<int(sample.stick_y)<<','
            <<int(sample.pc_mode)<<','<<sample.look_x<<','<<sample.look_y<<','<<int(sample.modern_flags)<<','
            <<int(sample.menu_kind)<<','<<int(sample.menu_target)<<'\n';
    }
    return serialized.str();
}

void test_ground_movement_recording() {
    // 0xA0: Modern actions with Run toward input; 0x80: Face the camera.
    const TemporaryReplay valid("jfg-phase8-input-v9\n"
        "1,2,1,0,0,80,3,4,0,160,0,0\n"
        "2,3,1,0,0,80,5,0,0,128,0,0\n"
        "3,4,1,0,-80,0,3,0,0,175,0,0\n"
        "4,5,1,0,0,0,0,0,0,0,0,0\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(valid.path(),replay)==jfg::InputReplayError::none,
        "v9 recordings carry the Ground movement bit");
    check(replay.sample_by_poll(0).modern_flags==(jfg::kModernActionsFlag|jfg::kModernRunTowardInputFlag) &&
        replay.sample_by_poll(1).modern_flags==jfg::kModernActionsFlag,"v9 keeps the bit both set and clear");
    check((replay.sample_by_poll(2).modern_flags&7U)==7U &&
        jfg::modern_roll_side_buttons(replay.sample_by_poll(2).modern_flags)==2 &&
        (replay.sample_by_poll(2).modern_flags&jfg::kModernRunTowardInputFlag)!=0,
        "the Ground movement bit coexists with scope and roll flags");
    const auto before=replay.events();
    const TemporaryReplay roundtrip(serialize_v9(before));
    check(jfg::DeterministicInputReplay::load(roundtrip.path(),replay)==jfg::InputReplayError::none &&
        replay.events()==before,"v9 serialization roundtrips the Ground movement bit");
    for(const auto *row:{"1,2,1,0,0,0,3,0,0,32,0,0","1,2,0,0,0,0,0,0,0,160,0,0"}) {
        const TemporaryReplay bad(std::string("jfg-phase8-input-v9\n")+row+"\n");
        check(jfg::DeterministicInputReplay::load(bad.path(),replay)==jfg::InputReplayError::invalid_record &&
            replay.events()==before,"v9 Ground movement needs Modern identity and a connection");
    }
    for(const auto *old:{"jfg-phase8-input-v5\n1,2,1,0,0,0,3,0,0,160\n",
        "jfg-phase8-input-v6\n1,2,1,0,0,0,3,0,0,160,0,0\n","jfg-phase8-input-v7\n1,2,1,0,0,0,3,0,0,160,0,0\n",
        "jfg-phase8-input-v8\n1,2,1,0,0,0,3,0,0,160,0,0\n"}) {
        const TemporaryReplay bad(old);
        check(jfg::DeterministicInputReplay::load(bad.path(),replay)==jfg::InputReplayError::invalid_record,
            "v5-v8 recordings cannot carry the Ground movement bit");
    }
    const TemporaryReplay fallback("jfg-phase8-input-v8\n1,2,1,0,0,80,3,0,0,128,0,0\n");
    check(jfg::DeterministicInputReplay::load(fallback.path(),replay)==jfg::InputReplayError::none &&
        replay.sample_at(1).modern_flags==jfg::kModernActionsFlag,"v8 Modern recordings still load unchanged");
}

std::string replay_ground_trace(const jfg::DeterministicInputReplay& replay) {
    jfg::ReplayModernControls modern;
    std::string trace;
    for(std::uint64_t poll=0;poll<replay.events().size();++poll) {
        (void)modern.observe(replay.sample_by_poll(poll));
        trace+=modern.ground_movement()==jfg::ModernGroundMovement::run_toward_input?'R':'F';
    }
    return trace;
}

void test_replay_ground_movement_follows_recording() {
    // Release-wait (flag 0), disconnected and Controller Mapping samples carry
    // no Ground movement value, so the last Modern sample's value holds.
    const TemporaryReplay recording("jfg-phase8-input-v9\n"
        "1,2,1,0,0,80,3,0,0,160,0,0\n"
        "2,3,1,0,0,0,0,0,0,0,0,0\n"
        "3,4,0,0,0,0,0,0,0,0,0,0\n"
        "4,5,1,0,0,80,5,0,0,128,0,0\n"
        "5,6,1,8000,0,0,0,0,0,0,0,0\n"
        "6,7,1,0,0,80,3,0,0,161,0,0\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(recording.path(),replay,jfg::InputReplayOrder::recorded_poll)==
        jfg::InputReplayError::none,"v9 Ground movement recording loads");
    check(jfg::ReplayModernControls{}.ground_movement()==jfg::ModernGroundMovement::face_camera,
        "replayed Ground movement starts as Face the camera");
    check(replay_ground_trace(replay)=="RRRFFR",
        "replayed Ground movement follows the recorded bit and holds across samples without Modern identity");

    const TemporaryReplay older("jfg-phase8-input-v8\n"
        "1,2,1,0,0,80,3,0,0,128,0,0\n"
        "2,3,1,0,-80,0,5,0,0,143,0,0\n");
    check(jfg::DeterministicInputReplay::load(older.path(),replay,jfg::InputReplayOrder::recorded_poll)==
        jfg::InputReplayError::none,"v8 Modern recording loads");
    check(replay_ground_trace(replay)=="FF","v8 Modern recordings replay as Face the camera");
}

void test_recorded_ground_movement_replays_as_recorded() {
    using jfg::ModernGroundMovement;
    for(const auto recorded:{ModernGroundMovement::run_toward_input,ModernGroundMovement::face_camera}) {
        // A recorder poll with scope cycle held, then one waiting for device release.
        jfg::ModernControlsConfig profile;profile.ground_movement=recorded;
        std::vector<jfg::InputReplayEvent> events(2);
        events[0]={1,2,{true,0,0,80,3,0,0,jfg::modern_recorded_flags(0x81U,profile)}};
        events[1]={2,3,{true,0,0,0,0,0,0,jfg::modern_recorded_flags(0U,profile)}};
        check(events[1].sample.modern_flags==0U,"samples without Modern identity record no Ground movement");
        check((events[0].sample.modern_flags&0x81U)==0x81U,"recording Ground movement keeps the live Modern flags");
        const TemporaryReplay recording(serialize_v9(events));
        jfg::DeterministicInputReplay replay;
        check(jfg::DeterministicInputReplay::load(recording.path(),replay,jfg::InputReplayOrder::recorded_poll)==
            jfg::InputReplayError::none,"recorder Ground movement output loads as v9");
        const std::string expected=recorded==ModernGroundMovement::run_toward_input?"RR":"FF";
        check(replay_ground_trace(replay)==expected,"playback uses the recorded Ground movement");
    }
}

void test_original_acceleration_recording() {
    // 0xC0: Modern actions with Original movement acceleration on; 0x80: off.
    // The bit records the option itself, so it is valid on any pc_mode.
    const TemporaryReplay valid("jfg-phase8-input-v9\n"
        "1,2,1,0,0,80,3,4,0,192,0,0\n"
        "2,3,1,0,0,80,3,0,0,128,0,0\n"
        "3,4,1,0,-80,0,3,0,0,255,0,0\n"
        "4,5,1,0,0,0,0,0,0,192,0,0\n"
        "5,6,1,0,0,0,0,0,0,0,0,0\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(valid.path(),replay)==jfg::InputReplayError::none,
        "v9 recordings carry the Original movement acceleration bit");
    check(replay.sample_by_poll(0).modern_flags==(jfg::kModernActionsFlag|jfg::kModernOriginalAccelerationFlag) &&
        replay.sample_by_poll(1).modern_flags==jfg::kModernActionsFlag,"v9 keeps the acceleration bit both set and clear");
    const auto all_flags=replay.sample_by_poll(2).modern_flags;
    check((all_flags&7U)==7U && (all_flags&jfg::kModernRollFlags)==jfg::kModernRollFlags &&
        (all_flags&jfg::kModernRunTowardInputFlag)!=0 && (all_flags&jfg::kModernOriginalAccelerationFlag)!=0,
        "the acceleration bit coexists with scope, roll and Ground movement flags");
    check(replay.sample_by_poll(3).pc_mode==0 &&
        (replay.sample_by_poll(3).modern_flags&jfg::kModernOriginalAccelerationFlag)!=0,
        "the acceleration bit is valid outside pc_mode 3");
    const auto before=replay.events();
    const TemporaryReplay roundtrip(serialize_v9(before));
    check(jfg::DeterministicInputReplay::load(roundtrip.path(),replay)==jfg::InputReplayError::none &&
        replay.events()==before,"v9 serialization roundtrips the acceleration bit");
    for(const auto *row:{"1,2,1,0,0,0,3,0,0,64,0,0","1,2,0,0,0,0,0,0,0,192,0,0","1,2,0,0,0,0,0,0,0,64,0,0"}) {
        const TemporaryReplay bad(std::string("jfg-phase8-input-v9\n")+row+"\n");
        check(jfg::DeterministicInputReplay::load(bad.path(),replay)==jfg::InputReplayError::invalid_record &&
            replay.events()==before,"v9 acceleration bit needs Modern identity and a connection");
    }
    for(const auto *old:{"jfg-phase8-input-v5\n1,2,1,0,0,0,3,0,0,192\n",
        "jfg-phase8-input-v6\n1,2,1,0,0,0,3,0,0,192,0,0\n","jfg-phase8-input-v7\n1,2,1,0,0,0,3,0,0,192,0,0\n",
        "jfg-phase8-input-v8\n1,2,1,0,0,0,3,0,0,192,0,0\n"}) {
        const TemporaryReplay bad(old);
        check(jfg::DeterministicInputReplay::load(bad.path(),replay)==jfg::InputReplayError::invalid_record,
            "v5-v8 recordings cannot carry the acceleration bit");
    }
}

std::string replay_acceleration_trace(const jfg::DeterministicInputReplay& replay) {
    jfg::ReplayModernControls modern;
    std::string trace;
    for(std::uint64_t poll=0;poll<replay.events().size();++poll) {
        (void)modern.observe(replay.sample_by_poll(poll));
        trace+=modern.original_acceleration()?'A':'-';
    }
    return trace;
}

void test_replay_original_acceleration_follows_recording() {
    // Release-wait (flag 0), disconnected and Controller Mapping samples carry
    // no option value, so the last Modern sample's value holds.
    const TemporaryReplay recording("jfg-phase8-input-v9\n"
        "1,2,1,0,0,80,3,0,0,192,0,0\n"
        "2,3,1,0,0,0,0,0,0,0,0,0\n"
        "3,4,0,0,0,0,0,0,0,0,0,0\n"
        "4,5,1,0,0,80,3,0,0,128,0,0\n"
        "5,6,1,8000,0,0,0,0,0,0,0,0\n"
        "6,7,1,0,0,80,3,0,0,224,0,0\n");
    jfg::DeterministicInputReplay replay;
    check(jfg::DeterministicInputReplay::load(recording.path(),replay,jfg::InputReplayOrder::recorded_poll)==
        jfg::InputReplayError::none,"v9 acceleration recording loads");
    check(!jfg::ReplayModernControls{}.original_acceleration(),"replayed acceleration starts off");
    check(replay_acceleration_trace(replay)=="AAA--A",
        "replayed acceleration follows the recorded bit and holds across samples without Modern identity");

    const TemporaryReplay older("jfg-phase8-input-v8\n"
        "1,2,1,0,0,80,3,0,0,128,0,0\n"
        "2,3,1,0,-80,0,5,0,0,143,0,0\n");
    check(jfg::DeterministicInputReplay::load(older.path(),replay,jfg::InputReplayOrder::recorded_poll)==
        jfg::InputReplayError::none,"v8 Modern recording loads");
    check(replay_acceleration_trace(replay)=="--","v8 Modern recordings replay without acceleration");
}

void test_recorded_original_acceleration_replays_as_recorded() {
    using jfg::ModernGroundMovement;
    for(const auto ground:{ModernGroundMovement::run_toward_input,ModernGroundMovement::face_camera}) {
        for(const bool accelerated:{true,false}) {
            jfg::ModernControlsConfig profile;
            profile.ground_movement=ground;
            profile.mouse_original_acceleration=accelerated;
            // A recorder poll with scope cycle held, then one waiting for device release.
            std::vector<jfg::InputReplayEvent> events(2);
            events[0]={1,2,{true,0,0,80,3,0,0,jfg::modern_recorded_flags(0x81U,profile)}};
            events[1]={2,3,{true,0,0,0,0,0,0,jfg::modern_recorded_flags(0U,profile)}};
            check(events[1].sample.modern_flags==0U,"samples without Modern identity record no acceleration");
            check((events[0].sample.modern_flags&0x81U)==0x81U,"recording acceleration keeps the live Modern flags");
            const TemporaryReplay recording(serialize_v9(events));
            jfg::DeterministicInputReplay replay;
            check(jfg::DeterministicInputReplay::load(recording.path(),replay,jfg::InputReplayOrder::recorded_poll)==
                jfg::InputReplayError::none,"recorder acceleration output loads as v9");
            check(replay_acceleration_trace(replay)==(accelerated?"AA":"--"),
                "playback uses the recorded acceleration option");
            const std::string expected_ground=ground==ModernGroundMovement::run_toward_input?"RR":"FF";
            check(replay_ground_trace(replay)==expected_ground,"the acceleration bit leaves the recorded Ground movement intact");
        }
    }
}

void test_replay_modern_controls_report_any_held_change() {
    const auto modern_sample=[](const std::uint8_t flags) {
        return jfg::ControllerReplaySample{true,0,0,0,3,0,0,static_cast<std::uint8_t>(jfg::kModernActionsFlag|flags)};
    };
    jfg::ReplayModernControls modern;
    check(modern.observe(modern_sample(0U)) && modern.active(),"turning Modern on is a change");
    check(!modern.observe(modern_sample(0U)),"repeating the held values is no change");
    check(modern.observe(modern_sample(jfg::kModernRunTowardInputFlag)) &&
        modern.ground_movement()==jfg::ModernGroundMovement::run_toward_input,
        "a Ground movement change alone is a change");
    check(!modern.observe(modern_sample(jfg::kModernRunTowardInputFlag)),"repeating Run toward input is no change");
    check(modern.observe(modern_sample(jfg::kModernRunTowardInputFlag|jfg::kModernOriginalAccelerationFlag)) &&
        modern.original_acceleration(),"an Original movement acceleration change alone is a change");
    check(!modern.observe(jfg::ControllerReplaySample{}),"a neutral flag-0 sample is no change");
}

}  // namespace

int main() {
    test_valid_replay();
    test_independent_look();
    test_precise_stick_recording();
    test_modern_action_recording();
    test_modern_roll_recording();
    test_native_menu_recording();
    test_long_manual_recording();
    test_connection_transitions();
    test_poll_replay_eof_is_neutral();
    test_repeated_poll_records();
    test_rejects_invalid_records();
    test_replay_modern_controls_follow_recording();
    test_replay_modern_controls_hold_until_mapping_input();
    test_replay_modern_controls_switch_mid_recording();
    test_replay_modern_controls_old_formats_stay_off();
    test_ground_movement_recording();
    test_replay_ground_movement_follows_recording();
    test_recorded_ground_movement_replays_as_recorded();
    test_original_acceleration_recording();
    test_replay_original_acceleration_follows_recording();
    test_recorded_original_acceleration_replays_as_recorded();
    test_replay_modern_controls_report_any_held_change();
    return failures == 0 ? 0 : 1;
}
