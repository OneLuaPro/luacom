package = "LuaCOM"
version = "scm.moteus-1"
source = {
  url = "https://github.com/moteus/luacom/archive/master.zip",
  dir = "luacom-master",
}
description = {
   summary = "Use COM libraries from Lua",
   detailed = [[
LuaCOM is an add-on library to the Lua language that allows Lua programs to use and implement objects that follow Microsoft's Component Object Model (COM) specification and use the ActiveX technology for property access and method calls.   ]],
   license = "MIT/X11",
   homepage = "http://luaforge.net/projects/luacom/"
}
dependencies = {
   "lua >= 5.1"
}
build = {
   platforms = {
      win32 = {
         type = "make",
         build_variables = {
            CFLAGS     = "$(CFLAGS)",
            LIBFLAG    = "$(LIBFLAG)",
            LUA_LIBDIR = "$(LUA_LIBDIR)",
            LUA_BINDIR = "$(LUA_BINDIR)",
            LUA_INCDIR = "$(LUA_INCDIR)",
            LUALIB     = "$(LUALIB)",
            LUA        = "$(LUA)",
         },
         build_target = "lc_dll",
         install_variables = {
            LUA_LIBDIR = "$(LIBDIR)",
         }
      }
   }
}
