cmake_minimum_required(VERSION 3.24)
project(f117enhanced C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Faithful port of F-117A Nighthawk Stealth Fighter 2.0 (SETUP, MPS_LOGO, PLAYER, START, VGAME, END and
# DSWAP in one process, F117.COM's chain loop in src/prog.c), VGA mode 13h with AdLib sound
# (port/spec/platform.md). The game's data and executables are read from the game folder at run time.

find_package(SDL3 REQUIRED CONFIG)

# Nuked-OPL3 (LGPL-2.1), built as its own library and linked in; see third_party/nuked-opl3/README.md
# (the repository's, one level up).
add_library(nuked_opl3 STATIC ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nuked-opl3/opl3.c)
target_include_directories(nuked_opl3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/nuked-opl3)

# Coordinator files in src/, then one glob per module directory (PORTING.md "Source layout"): a module adds
# <module>/<file>.c and it is built without touching this file. programs/ holds the phase 4 placeholders.
set(F117_MODULES platform video sound frontend shell sim render3d flight debrief programs enhanced)
file(GLOB F117_SOURCES CONFIGURE_DEPENDS src/*.c)
foreach(m ${F117_MODULES})
    file(GLOB m_sources CONFIGURE_DEPENDS src/${m}/*.c)
    list(APPEND F117_SOURCES ${m_sources})
endforeach()

add_executable(f117 ${F117_SOURCES})
# src/gen holds the generated headers (symbols_<image>.h, module_<spec>.h; PORTING.md "Symbol pipeline"), committed
# and checked by the gen_*_current tests below. Both "x.h" and "gen/x.h" resolve.
target_include_directories(f117 PRIVATE src src/gen)
set_target_properties(f117 PROPERTIES OUTPUT_NAME f117-enhanced)   # ENH: the Enhanced executable
target_link_libraries(f117 PRIVATE SDL3::SDL3 nuked_opl3)
if(MSVC)
    target_compile_options(f117 PRIVATE /W3)
else()
    # The memory model reads multi-byte values through byte pointers (see src/mem.h).
    target_compile_options(f117 PRIVATE -Wall -Wextra -fno-strict-aliasing)
endif()
if(WIN32)
    # SDL3.dll, then what it loads in turn (MSYS2's SDL3.dll needs libiconv-2.dll), found where the compiler's
    # own DLLs are. Without it Windows takes whatever libiconv-2.dll is on the PATH; a 32-bit one fails with
    # 0xc000007b.
    get_filename_component(F117_DLL_DIR "${CMAKE_C_COMPILER}" DIRECTORY)
    add_custom_command(TARGET f117 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:f117> $<TARGET_FILE_DIR:f117>
        COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:f117> "-DSEARCH=${F117_DLL_DIR}"
                -DCMAKE_OBJDUMP=${CMAKE_OBJDUMP} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/copy_dlls.cmake
        COMMAND_EXPAND_LISTS)
endif()

# Tests (ctest): the chain with the placeholder programs, the tick source, and the game-folder check.
enable_testing()
add_test(NAME chain COMMAND f117 --headless --chain-test)
set_tests_properties(chain PROPERTIES PASS_REGULAR_EXPRESSION "chain-test OK")
add_test(NAME keymap COMMAND f117 --keymap-test)
set_tests_properties(keymap PROPERTIES PASS_REGULAR_EXPRESSION "keymap-test OK")
add_test(NAME tick_rate COMMAND f117 --headless --tick-test 1000)
# 70.09 Hz: 70 or 71 ticks in a second; a loaded machine may see a few fewer, never more than 72.
set_tests_properties(tick_rate PROPERTIES PASS_REGULAR_EXPRESSION "ticks in 1000 ms: (6[4-9]|7[0-2]) ")
add_test(NAME check_missing_files COMMAND f117 --check --game-dir ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(check_missing_files PROPERTIES WILL_FAIL TRUE)
# The platform layer (src/platform, S5.1): RTL against vectors computed from the original under Unicorn
# (port/notes/s51/platform_emu.py), DOS files in a scratch folder, keyboard scan codes, joystick scaling, timer; and the
# loader on the real programs in ../Game (skipped, code 77, when the game files are not there).
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/platform_test_dir)
add_test(NAME platform COMMAND f117 --headless --platform-test --game-dir ${CMAKE_CURRENT_BINARY_DIR}/platform_test_dir)
set_tests_properties(platform PROPERTIES PASS_REGULAR_EXPRESSION "platform-test OK")
add_test(NAME exe_load COMMAND f117 --headless --exe-test --game-dir ${CMAKE_CURRENT_SOURCE_DIR}/../Game)
set_tests_properties(exe_load PROPERTIES SKIP_RETURN_CODE 77)

# The video module (src/video, S5.2): the driver primitives bit-exact against the ORIGINAL MGRAPHIC under Unicorn (vectors from
# port/notes/s52/video_emu.py; text scenarios are skipped without ../Game/MGRAPHIC.EXE), every PIC / PAN / PAL / font of ../Game
# against the Phase 2 Python decoders (port/notes/s52/decoder_ref.py), and the composed screen (--video-test writes PNGs to
# work/s52/screen for the visual check U-video). The two that need the game files skip with code 77 without them.
add_test(NAME video_selftest COMMAND f117 --headless --video-selftest --game-dir ${CMAKE_CURRENT_SOURCE_DIR}/../Game)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../work/s52/screen)
add_test(NAME video_screen COMMAND f117 --headless --video-test --game-dir ${CMAKE_CURRENT_SOURCE_DIR}/../Game)
set_tests_properties(video_screen PROPERTIES SKIP_RETURN_CODE 77
                     ENVIRONMENT "F117_SNAPSHOT_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../work/s52/screen")
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME video_decoders COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/s52/decoder_ref.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/decoder_ref)
    set_tests_properties(video_decoders PROPERTIES SKIP_RETURN_CODE 77)
endif()

# The front end (src/frontend, S5.3): SETUP, MPS_LOGO and PLAYER against the ORIGINAL programs run under Unicorn (port/notes/s53/frontend_ref.py:
# the text screens and the comm block of SETUP, the screen + DAC CRC of each of the 82 logo frames) and the Phase 2 PAN decoder (every cel of every
# .PAN through PLAYER's ring buffer), the fade formula, the 60.08 Hz animation clock and the logo pacing; `frontend_chain` is the scripted run
# (F117_KEYS answers SETUP, keys end the PLAYER runs) through the chain up to the START placeholder. Both skip (77) without ../Game.
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME frontend COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/s53/frontend_ref.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/frontend_ref)
    add_test(NAME frontend_chain COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/s53/frontend_ref.py --chain
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game)
    set_tests_properties(frontend frontend_chain PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 150)
endif()

# The shell (src/shell, S5.4): the ORIGINAL START.EXE under Unicorn (port/notes/s54/shell_emu.py) runs two scripts (pilot creation, options, orders,
# briefing map, orders pages, arming, launch; a roster read and saved again); the port runs them on a scratch copy of the game folder and must give
# the same screen + DAC CRC at every step, the same comm block (the generated mission, byte for byte) and the same ROSTER.FIL. `shell_chain` is the
# scripted run of the whole chain (F117_KEYS for SETUP and the intro, F117_SHELL_SCRIPT for START): new pilot, strike mission, launch, VGAME and
# END placeholders, START again, quit. Both skip (77) without ../Game.
if(Python3_Interpreter_FOUND)
    add_test(NAME shell COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/s54/shell_ref.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/shell_ref)
    add_test(NAME shell_chain COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/s54/shell_ref.py --chain
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game)
    set_tests_properties(shell shell_chain PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600)
endif()

# The sim flow (src/sim, S5.5): VGAME's start-up state, the ISR body and 1262 function-call scenarios against the ORIGINAL VGAME.EXE under Unicorn
# (port/notes/s55/sim_ref.py: the whole DGROUP, the three pages row by row, the DAC and the comm block after every scenario; render3d, flight, weapons,
# AI and damage are stubs on both sides). It needs the comm block of the `shell` test (or F117_COMM). `shell_chain` also runs the real VGAME (a few
# frames, F117_VGAME_FRAMES). Skips (77) without ../Game.
if(Python3_Interpreter_FOUND)
    add_test(NAME sim_vgame COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/s55/sim_ref.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/sim_ref)
    set_tests_properties(sim_vgame PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 DEPENDS shell)
endif()

# The render3d leaf routines (src/render3d, S5.6a): the 12EF clipper, the 1359 raster back end, segment 1001's hz_draw, the 1442 sky/ground bands and the 1433 maths
# against the Python references of port/notes/s37 (R7b_ref.py, R7c_ref.py; 0 mismatches against the original under Unicorn) on thousands of random cases per routine
# (port/notes/s56/r3d_leaf_ref.py writes the vectors, `f117 --r3d-test` runs them). Needs work/unpacked/VGAME.EXE for the tables; skips (77) without it.
if(Python3_Interpreter_FOUND)
    add_test(NAME render3d_leaf COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/s56/r3d_leaf_ref.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_BINARY_DIR}/r3d_leaf)
    set_tests_properties(render3d_leaf PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 PASS_REGULAR_EXPRESSION "r3d-test OK")
endif()

# The whole 3D renderer (src/render3d, S5.6b): golden frames. The ORIGINAL VGAME.EXE under Unicorn (port/notes/s56/r3d_golden.py on top of the S5.5 harness) and
# `f117 --render-test` run the same scenes from a running mission (r3d_scene_draw on the runway, low, high, at targets, inside models, external views, whole
# view_frame calls after sim steps, the map MFD terrain at every zoom, the TV camera, the collision probe) and compare DGROUP, the pages, the DAC and the comm
# block byte for byte. References cached in the build tree (stamp of the scripts, VGAME.EXE, the comm block). Needs the `shell` comm block; skips (77) without ../Game.
if(Python3_Interpreter_FOUND)
    add_test(NAME render3d_golden COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/s56/r3d_golden.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/r3d_golden)
    set_tests_properties(render3d_golden PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 DEPENDS shell)
endif()

# The debrief (src/debrief, S5.8): the ORIGINAL END.EXE under Unicorn (port/notes/s58/end_emu.py) replays eight mission outcomes (landed with kills and objectives, ejected and
# rescued, ejected and captured, crashed, promoted with medals, a training flight, the 99th mission lost and won) from comm blocks built on the `shell` test's mission; the port
# runs the same scripts and must give the same screen + DAC CRC at every step, the same pilot record (the comm block, byte for byte) and the exit code 23h. `debrief_chain` is the
# scripted run of the whole chain with the real END: a landing ends VGAME, END debriefs, START saves a roster that holds END's record. References cached in the build tree.
# `debrief_chain_sea` is the user's crash of 2026-10-16: an Esc in flight, a dive into the sea off Tripoli, END (killed in action), START (no quit to the desktop).
# Both skip (77) without ../Game or the `shell` comm block.
if(Python3_Interpreter_FOUND)
    add_test(NAME debrief COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/s58/end_ref.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/debrief_ref)
    add_test(NAME debrief_chain COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/s58/end_ref.py --chain
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game)
    add_test(NAME debrief_chain_sea COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/s58/end_ref.py --chain-sea
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game)
    set_tests_properties(debrief debrief_chain debrief_chain_sea PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 DEPENDS shell)
endif()

# The sound module (src/sound, S5.9): ASOUND.117 and ASOUND.LOG ported from sound.md. `sound` replays scripts of driver calls (every single command, the PLAYER intro
# soundtrack, the debrief music, mixes of the VGAME / shell / END effects with the engine drone, the logo jingle) recorded from the ORIGINAL drivers under Unicorn
# (port/notes/s59/sound_ref.py) and compares every OPL register write and every snd_poll result (0 differences), and checks the poll rates of the program ISR bodies
# (START 70.09 Hz; PLAYER, END, VGAME 60.08 Hz with the 7th-tick skip; the logo overlay 60.88 Hz). `sound_chain` runs the front-end chain with the driver on SDL's dummy
# audio and video drivers (the launcher's Sound checkbox) and no --headless. Both skip (77) without ../Game or unicorn.
if(Python3_Interpreter_FOUND)
    add_test(NAME sound COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/s59/sound_ref.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/sound_ref)
    add_test(NAME sound_chain COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/s59/sound_ref.py --chain
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game)
    set_tests_properties(sound sound_chain PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)
endif()

# The generated headers are current: the generators re-run in memory and fail on any difference (tools/gen_*.py).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME gen_symbols_current COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_symbols.py --check)
    add_test(NAME gen_module_headers_current COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../tools/gen_module_headers.py --check)
else()
    message(WARNING "Python 3 not found: the gen_*_current tests are not registered")
endif()


# ENH: S2.1, the canvas geometry and the compositor (no game files needed): with empty layers the canvas equals the classic frame scaled N x N.
add_test(NAME compose COMMAND f117 --compose-test)
set_tests_properties(compose PROPERTIES PASS_REGULAR_EXPRESSION "compose-test OK")

# ENH: S2.2, the hr 3D back end on the render3d golden scenes, enhancements ON (port/notes/e22/r3d_hr.py): the golden frames stay byte for byte, the hr
# 3D layer equals the 1x page at --render-scale 1 and agrees with it away from edges at 2, 3, 4. Uses the golden test's cached references.
if(Python3_Interpreter_FOUND)
    add_test(NAME r3d_hr_n1 COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/e22/r3d_hr.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/r3d_hr_n1 ${CMAKE_CURRENT_BINARY_DIR}/r3d_golden 1)
    add_test(NAME r3d_hr_n234 COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/e22/r3d_hr.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/r3d_hr_n234 ${CMAKE_CURRENT_BINARY_DIR}/r3d_golden 2 3 4)
    # ENH: S2.3, the independent reference rasteriser (port/notes/e23/r3d_ref.py) against the hr layer at N = 2 and 4, both dither modes, incl. the
    # patched-table dither / stipple scenes; and the PNG / mask dump tool on one scene (the files go to work/e23 when run by hand).
    add_test(NAME r3d_hr_ref COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/e23/r3d_ref.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/r3d_hr_ref ${CMAKE_CURRENT_BINARY_DIR}/r3d_golden 2 4)
    add_test(NAME r3d_hr_dump COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/e23/dump_scenes.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/r3d_golden ${CMAKE_CURRENT_BINARY_DIR}/r3d_hr_dump --scenes dither_close0,runway0)
    # S3.2: the TV camera and map MFD windows cut out of the frame at N = 4 (classic vs hr PNGs and contact_tv_map_scenes.png of the poked scenes; the files go to work/e32 when run by hand)
    add_test(NAME r3d_hr_tvmap COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/e23/dump_scenes.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/r3d_golden ${CMAKE_CURRENT_BINARY_DIR}/r3d_hr_tvmap --tvmap --n 4)
    set_tests_properties(r3d_hr_tvmap PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600 DEPENDS render3d_golden PASS_REGULAR_EXPRESSION "dump-scenes: .*contact_tv_map_scenes.png")
    # S3.2 fix: the REAL game path (port/notes/e32/real_path.py): the scripted chain into a Libya mission, the right MFD on the TV page showing the home airbase, the left on
    # the map; after every page flip how much of the two windows is shown from the 3D layer (--mfd-probe). By hand: add `N --png work/e32` for contact_tv_map.png.
    add_test(NAME r3d_hr_realpath COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/e32/real_path.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/r3d_hr_realpath 4)
    set_tests_properties(r3d_hr_realpath PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 PASS_REGULAR_EXPRESSION "r3d-realpath OK")
    set_tests_properties(r3d_hr_ref PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 DEPENDS render3d_golden PASS_REGULAR_EXPRESSION "r3d-ref OK")
    set_tests_properties(r3d_hr_dump PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 600 DEPENDS render3d_golden PASS_REGULAR_EXPRESSION "dump-scenes: .*contact_selected.png")
    set_tests_properties(r3d_hr_n1 r3d_hr_n234 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 DEPENDS render3d_golden PASS_REGULAR_EXPRESSION "r3d-hr OK")
endif()

# ENH: S2.4, the sandboxed display pass (port/notes/e24/sandbox_hash.py): the scripted chain of debrief_chain_sea on the virtual clock (F117_VCLOCK), once
# plain, once with 3 display passes per VGAME frame (another view per pass), once in --classic; the per-frame hash of the sim state (DGROUP, comm block, LCG,
# sound driver, OPL stream, mem[], VGA, canvas) must be identical every frame, and the final comm block and roster too, with no forbidden call in a pass.
if(Python3_Interpreter_FOUND)
    add_test(NAME enh_sandbox_hash COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/e24/sandbox_hash.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/enh_sandbox 3)
    set_tests_properties(enh_sandbox_hash PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 PASS_REGULAR_EXPRESSION "enh_sandbox_hash OK")
endif()

# ENH: S4.1, the display stream (port/notes/e41/interp_equal.py). sim_interp_equal: the chain of enh_sandbox_hash in a wide window with --fps off, --fps-test 4,
# --fps 60, --fps vsync (and night + smooth shading with --fps-test 3 against --fps off): every frame hash, the final comm block, OPL stream and roster identical.
# enh_fps_jerk: the real-path view sweep in banked flight with --fps-test 4 --fps-trace: the camera of the own-aircraft views steps evenly between refreshes,
# never backward, holding frames show one picture; then a real-time run with --fps 60 (alpha from the display clock).
if(Python3_Interpreter_FOUND)
    add_test(NAME sim_interp_equal COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/e41/interp_equal.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/enh_interp equal)
    set_tests_properties(sim_interp_equal PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1500 PASS_REGULAR_EXPRESSION "sim_interp_equal OK")
    add_test(NAME enh_fps_jerk COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/e41/interp_equal.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/enh_jerk jerk)
    set_tests_properties(enh_fps_jerk PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1500 PASS_REGULAR_EXPRESSION "enh_fps_jerk OK")
endif()

# ENH: S3.4, the wider view. enh_wide_geometry: the placement of every view for 16:9, 21:9, 32:9, 4:3, 16:10, tall and small windows (no game files needed):
# the first cockpit view and the outside views extended (frame bottom centre, viewport = canvas), the second cockpit view and left / right / rear not (frame centred).
# r3d_hr_wide_realpath (port/notes/e34/wide_realpath.py): the scripted chain with a view sweep at 16:9 and 21:9, per view what the presented canvas shows
# (By hand: add `--png work/e34` for wide_<aspect>_<view>.png and contact_wide.png.) The sandbox hash test above has a --wide run (the sim unchanged).
add_test(NAME enh_wide_geometry COMMAND f117 --wide-geometry-test)
set_tests_properties(enh_wide_geometry PROPERTIES PASS_REGULAR_EXPRESSION "wide-geometry OK")
if(Python3_Interpreter_FOUND)
    add_test(NAME r3d_hr_wide_realpath COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/e34/wide_realpath.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/r3d_hr_wide_realpath)
    set_tests_properties(r3d_hr_wide_realpath PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 PASS_REGULAR_EXPRESSION "wide-realpath OK")
    # r3d_hr_wide (port/notes/e34/wide_scenes.py): the golden scenes with their main 3D pass drawn wide on a 16:9-like and a 21:9-like canvas at N = 2: the reference
    # rasteriser on the whole layer, no unwritten pixel in the extended world, nothing in the panel hole, the frame equal to the frame-only render, no seam at the border.
    add_test(NAME r3d_hr_wide COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/e34/wide_scenes.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/r3d_hr_wide ${CMAKE_CURRENT_BINARY_DIR}/r3d_golden 2)
    set_tests_properties(r3d_hr_wide PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 DEPENDS render3d_golden PASS_REGULAR_EXPRESSION "r3d-wide OK")
endif()

# ENH: S3.5, smooth distance shading (--smooth-shading, DESIGN 2.11). r3d_hr_smooth (port/notes/e35/smooth_scenes.py): the golden scenes at N = 2, frame and 16:9 wide canvas,
# and at N = 1, option off against on: the layer index unchanged, untouched pixels identical, every blended pixel the mix of its two ramp colours from the live DAC, the
# band steps gone (monotonic along the columns). r3d_hr_smooth_realpath (port/notes/e35/smooth_realpath.py): the sandbox-hash chain day and night (F117_TEST_NIGHT) with the
# option: the sim identical; the view sweep of wide_realpath with the option, day and night: no crash, the same frames. (By hand: add `--png work/e35` to either.)
if(Python3_Interpreter_FOUND)
    add_test(NAME r3d_hr_smooth COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/e35/smooth_scenes.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/r3d_smooth ${CMAKE_CURRENT_BINARY_DIR}/r3d_golden 2)
    set_tests_properties(r3d_hr_smooth PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 DEPENDS render3d_golden PASS_REGULAR_EXPRESSION "r3d-smooth OK")
    add_test(NAME r3d_hr_smooth_realpath COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/e35/smooth_realpath.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/r3d_smooth_realpath)
    set_tests_properties(r3d_hr_smooth_realpath PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 PASS_REGULAR_EXPRESSION "r3d-smooth-realpath OK")
endif()

# ENH: S3.6, true night (--true-night, DESIGN 2.12). r3d_hr_night (port/notes/e36/night_scenes.py): the golden scenes as a day and as a night mission (palette B),
# option off against on (also with --smooth-shading, also on a 16:9 wide canvas): by day nothing changes; at night every pixel without a night factor is identical
# (HUD, MFDs, art, the TV and map scenes, sprites, lights), every darkened one is the option-off colour times the factor, the factors on the curve and monotonic down
# the band columns, the 3D world darker. r3d_hr_night_realpath (port/notes/e36/night_realpath.py): the sandbox-hash chain day, night (F117_TEST_NIGHT=2) and night
# with smooth shading, option off against on: the sim identical; the view sweep at night and by day: no crash, the same frames, darker at night, unchanged by day.
# (By hand: add `--png work/e36` to either.)
if(Python3_Interpreter_FOUND)
    add_test(NAME r3d_hr_night COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/e36/night_scenes.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/r3d_night ${CMAKE_CURRENT_BINARY_DIR}/r3d_golden 2)
    set_tests_properties(r3d_hr_night PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 DEPENDS render3d_golden PASS_REGULAR_EXPRESSION "r3d-night OK")
    add_test(NAME r3d_hr_night_realpath COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../port/notes/e36/night_realpath.py
             $<TARGET_FILE:f117> ${CMAKE_CURRENT_SOURCE_DIR}/../Game ${CMAKE_CURRENT_BINARY_DIR}/r3d_night_realpath)
    set_tests_properties(r3d_hr_night_realpath PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 1800 PASS_REGULAR_EXPRESSION "r3d-night-realpath OK")
endif()

# ENH: every faithful test runs with the enhancements off (F117_CLASSIC acts as --classic for the Python drivers' own command lines).
get_property(F117_ALL_TESTS DIRECTORY PROPERTY TESTS)
foreach(t ${F117_ALL_TESTS})
    if(NOT t STREQUAL "compose" AND NOT t MATCHES "^r3d_hr_" AND NOT t MATCHES "^enh_")
        set_property(TEST ${t} APPEND PROPERTY ENVIRONMENT F117_CLASSIC=1)
    endif()
endforeach()

# ENH: S5.1, the launcher (f117-enhanced-launcher.exe, wxWidgets 3.2, built beside f117-enhanced.exe). -DF117_LAUNCHER=OFF skips it.
option(F117_LAUNCHER "Also build the launcher, f117-enhanced-launcher (wxWidgets)" ON)
if(F117_LAUNCHER)
    add_subdirectory(launcher)
    if(Python3_Interpreter_FOUND)
        add_test(NAME launcher_args COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/launcher/test_args.py
                 $<TARGET_FILE:f117elauncher> ${CMAKE_CURRENT_BINARY_DIR}/launcher_args)
    endif()
endif()
