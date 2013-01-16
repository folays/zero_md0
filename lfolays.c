#include <stdlib.h>
#include <unistd.h>

#define lfolays_c
#define LUA_LIB

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"

static int folays_usleep(lua_State *L)
{
  int n = luaL_checkinteger(L, 1);
  usleep(n);
  return 0;
}

static const luaL_Reg folayslib[] = {
  {"usleep", folays_usleep},
  {NULL, NULL}
};

LUAMOD_API int luaopen_folays (lua_State *L) {
  luaL_newlib(L, folayslib);
  return 1;
}
