{
  "variables": {
    "asiosdkdir": "<!(node -e \"console.log(require('path').join('<(module_root_dir)', 'repos', 'ASIOSDK2.3'))\")",
    "asiosdkdir": "<(module_root_dir)/repos/ASIOSDK2.3",
    "portaudiodir": "<(module_root_dir)/repos/portaudio",
    "incdirs": ["<(portaudiodir)/include", "<(portaudiodir)/src/common"],
    "src_common": ["<!@(python -c \"import glob,sys,os,os.path; \
                        [sys.stdout.write(f.replace(os.sep, 2*os.sep) + os.linesep) \
                        for f in glob.glob(os.path.join('.', 'repos', 'portaudio', 'src', 'common', '*.[ch]'))]\")"],
    "src_hostapi_alsa": ['<(portaudiodir)/src/hostapi/alsa/pa_linux_alsa.c'],
    "src_hostapi_asihpi": ["<(portaudiodir)/src/hostapi/asihpi/pa_linux_asihpi.c"],
    "src_hostapi_asio": ["<(portaudiodir)/src/hostapi/asio/iasiothiscallresolver.cpp",
      "<(portaudiodir)/src/hostapi/asio/iasiothiscallresolver.h",
      "<(portaudiodir)/src/hostapi/asio/pa_asio.cpp"
    ],
    "src_hostapi_coreaudio": ["<(portaudiodir)/src/hostapi/coreaudio/pa_mac_core_blocking.c",
      "<(portaudiodir)/src/hostapi/coreaudio/pa_mac_core_blocking.h",
      "<(portaudiodir)/src/hostapi/coreaudio/pa_mac_core_internal.h",
      "<(portaudiodir)/src/hostapi/coreaudio/pa_mac_core_utilities.c",
      "<(portaudiodir)/src/hostapi/coreaudio/pa_mac_core_utilities.h",
      "<(portaudiodir)/src/hostapi/coreaudio/pa_mac_core.c"
    ],
    "src_hostapi_dsound": ["<(portaudiodir)/src/hostapi/dsound/pa_win_ds_dynlink.c",
      "<(portaudiodir)/src/hostapi/dsound/pa_win_ds_dynlink.h",
      "<(portaudiodir)/src/hostapi/dsound/pa_win_ds.c"
    ],
    "src_hostapi_jack": ["<(portaudiodir)/src/hostapi/jack/pa_jack.c"],
    "src_hostapi_oss": ["<(portaudiodir)/src/hostapi/oss/pa_unix_oss.c",
      "<(portaudiodir)/src/hostapi/oss/recplay.c"
    ],
    "src_hostapi_wasapi": ["<(portaudiodir)/src/hostapi/wasapi/pa_win_wasapi.c"],
    "src_hostapi_wdmks": ["<(portaudiodir)/src/hostapi/wdmks/pa_win_wdmks.c"],
    "src_hostapi_wmme": ["<(portaudiodir)/src/hostapi/wmme/pa_win_wmme.c"],
    "src_os_unix": ["<(portaudiodir)/src/os/unix/pa_unix_hostapis.c",
      "<(portaudiodir)/src/os/unix/pa_unix_util.c",
      "<(portaudiodir)/src/os/unix/pa_unix_util.h"
    ],
    "src_os_win": ["<!@(python -c \"import glob,sys,os,os.path; \
                        [sys.stdout.write(f.replace(os.sep, 2*os.sep) + os.linesep) \
                        for f in glob.glob(os.path.join('.', 'repos', 'portaudio', 'src', 'os', 'win', '*.[ch]'))]\")"],
    "src_asio": ["<(asiosdkdir)/common/asio.cpp",
      "<(asiosdkdir)/host/asiodrivers.cpp",
      "<(asiosdkdir)/host/pc/asiolist.cpp"
    ],
    "inc_asio": ["<(asiosdkdir)/common",
      "<(asiosdkdir)/host",
      "<(asiosdkdir)/host\pc"
    ]
  },
  "targets": [{
    "target_name": "portaudio_x64",
    "type": "shared_library",
    "sources": ["<@(src_common)"],
    "include_dirs": ["<@(incdirs)"],
    "defines": [],
    "cflags!": [],
    "cflags_cc!": [],
    "conditions": [
      [
        "OS=='linux'", {
          "libraries": [],
          "configurations": {
            "Debug": {
              'defines': ['DEBUG', '_DEBUG',
                'STACK_CHECK_BUILTIN',
                'STACK_CHECK_STATIC_BUILTIN'
              ],
              'cflags': ['-g', '-O0',
                '-fstack-check',
                '-fstack-protector-all',
                '-fstack-usage'
              ],
              'cflags_cc': ['-g', '-O0',
                '-fstack-check',
                '-fstack-protector-all',
                '-fstack-usage'
              ]
            },
            "Release": {}
          },
          "defines": []
        },

        "OS=='mac'", {

        },

        "OS=='win'", {
          "variables": {},
          "sources": [
            "<@(src_asio)",
            "<@(src_os_win)",
            "<@(src_hostapi_asio)",
            "<@(src_hostapi_wasapi)",
            "<@(src_hostapi_wdmks)",
            "<@(src_hostapi_wmme)"
          ],
          "defines": [
            "WIN32=1",
            "_USRDLL=1",
            "_CRT_SECURE_NO_DEPRECATE=1",
            "PAWIN_USE_WDMKS_DEVICE_INFO=1",
            "PA_USE_ASIO=1",
            "PA_USE_DS=0",
            "PA_USE_WMME=1",
            "PA_USE_WASAPI=1",
            "PA_USE_WDMKS=1"
          ],
          "include_dirs": [
            "<(portaudiodir)/src/os/win",
            "<@(inc_asio)"
          ],
          "link_settings": {
            "libraries": []
          },
          "configurations": {
            "Debug": {
              "defines": [
                "_DEBUG=1",
                "PA_ENABLE_DEBUG_OUTPUT=1"
              ],
              "msvs_settings": {
                "VCCLCompilerTool": {
                  "Optimization": 0,
                  "InlineFunctionExpansion": 1,
                  "EnableIntrinsicFunctions": "true",
                  "FavorSizeOrSpeed": 1,
                  "StringPooling": "true",
                  "RuntimeLibrary": 2,
                  "EnableFunctionLevelLinking": "true",
                  "WarningLevel": 3,
                  "SuppressStartupBanner": "true",
                  "AdditionalOptions": ["/EHsc"],
                  "MinimalRebuild": "true",
                  "BasicRuntimeChecks": 3,
                  "RuntimeLibrary": 3,
                  "DebugInformationFormat": 3
                },
                "VCLibrarianTool": {
                  "AdditionalOptions": []
                },
                "VCLinkerTool": {
                  "AdditionalDependencies": ["ksuser.lib"],
                  "LinkIncremental": 1,
                  "SuppressStartupBanner": "true",
                  "AdditionalLibraryDirectories": [],
                  "ModuleDefinitionFile": "<(portaudiodir)/build/msvc/portaudio.def"
                }
              }
            },
            "Release": {
              "defines": [
                "NDEBUG=1"
              ],
              "msvs_settings": {
                "VCCLCompilerTool": {
                  "Optimization": 2,
                  "InlineFunctionExpansion": 1,
                  "EnableIntrinsicFunctions": "true",
                  "FavorSizeOrSpeed": 1,
                  "StringPooling": "true",
                  "RuntimeLibrary": 2,
                  "EnableFunctionLevelLinking": "true",
                  "WarningLevel": 3,
                  "SuppressStartupBanner": "true",
                  "AdditionalOptions": ["/EHsc"]
                },
                "VCLibrarianTool": {
                  "AdditionalOptions": []
                },
                "VCLinkerTool": {
                  "AdditionalDependencies": ["ksuser.lib"],
                  "LinkIncremental": 1,
                  "SuppressStartupBanner": "true",
                  "AdditionalLibraryDirectories": [],
                  "ModuleDefinitionFile": "<(portaudiodir)/build/msvc/portaudio.def"
                }
              }
            }
          }
        }
      ]
    ]
  }]
}