set(DEBUGGER_SRC
  debugger.cpp
  debugger_disasm.cpp
  debugger_host.cpp
  debugger_gui.cpp
  LizardComms.g.cpp
)

target_sources(libdosboxcommon PRIVATE
  debugger.cpp
  debugger_disasm.cpp
  debugger_host.cpp
  debugger_gui.cpp
  LizardComms.g.cpp
)

target_link_libraries(libdosboxcommon PRIVATE libpdcurses)
