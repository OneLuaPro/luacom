
#include <windows.h>
#include <stdio.h>
extern "C" {
  #include "lua.h"
  #include "lualib.h"
  #include "lauxlib.h"
#ifdef IUP
  #include "iup.h"
  #ifndef NO_CPICONTROLS
  #include "iupcontrols.h"
  #include "iupgl.h"
  #include "iupluaim.h"
  #endif
  #ifdef SPEECH_SDK
  #include "iupspeech.h"
  #endif
  #include "iuplua.h"
  #include "cdlua5.h"
  #ifndef NO_CPICONTROLS
  #include "luacontrols.h"
  #include "luagl.h"
  #endif
  #ifdef SPEECH_SDK
  #include "luaspeech.h"
  #endif
  #ifdef USE_GLLUA
  #include "gllua.h"
  #endif
#endif
}
#include "LuaCompat.h"
#include "luacom.h"
#include "luacom_internal.h"
#include "tLuaCOMClassFactory.h"
#include "tLuaCOMException.h"
#include "tUtil.h"
#include "tCOMUtil.h"

// COM is initialized for the thread that opens luacom in a Lua state, not
// in DllMain (the loader lock forbids it there, and it reached only the
// thread that loaded the DLL). The state undoes its initialization when it
// is closed on the same thread.
struct ApartmentOwner {
  DWORD thread_id;
  bool initialized;
};

static const char apartment_key[] = "luacom_apartment";

static int apartment_gc(lua_State* L) {
  ApartmentOwner* owner = (ApartmentOwner*)lua_touserdata(L, 1);
  if(owner && owner->initialized &&
     owner->thread_id == GetCurrentThreadId()) {
    owner->initialized = false;
    OleUninitialize();
  }
  return 0;
}

static void initializeApartment(lua_State* L) {
  lua_getfield(L, LUA_REGISTRYINDEX, apartment_key);
  const bool present = !lua_isnil(L, -1);
  lua_pop(L, 1);
  if(present)
    return;

  // The owner is created before any LuaCOM object of the state, so
  // lua_close finalizes it after them. It is anchored before
  // OleInitialize, so a failed allocation leaves COM untouched.
  ApartmentOwner* owner =
    (ApartmentOwner*)lua_newuserdata(L, sizeof(ApartmentOwner));
  owner->thread_id = GetCurrentThreadId();
  owner->initialized = false;
  lua_newtable(L);
  lua_pushcfunction(L, apartment_gc);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  lua_setfield(L, LUA_REGISTRYINDEX, apartment_key);

  // STA, as before in DllMain. S_FALSE: the host has initialized the
  // thread already; the count is balanced at close all the same.
  // RPC_E_CHANGED_MODE: the host made the thread MTA; COM works there,
  // nothing is to be undone.
  const HRESULT hr = OleInitialize(NULL);
  if(hr == S_OK || hr == S_FALSE)
    owner->initialized = true;
  else if(hr != RPC_E_CHANGED_MODE) {
    char message[64];
    _snprintf(message, sizeof(message),
              "luacom: OleInitialize failed (HRESULT 0x%08X)",
              (unsigned int)hr);
    message[sizeof(message) - 1] = '\0';
    luaL_error(L, "%s", message);
  }
}

// luaopen_luacom and luacom_openlib initialize COM; a host that calls
// luacom_open or luacom_open_core directly initializes COM itself.
extern "C" int luacom_openlib(lua_State* L) {
  initializeApartment(L);
  luacom_open(L);
  lua_getglobal(L, LIBNAME);
  return 1;
}

extern "C" int luaopen_luacom(lua_State* L) {
  return luacom_openlib(L);
}


#define lua_dofile luaL_dofile


static lua_State* factoryCache; // Maps CLSIDs to factories
CRITICAL_SECTION cs_factoryCache;

static void factoryCache_Init() {
  InitializeCriticalSection(&cs_factoryCache);

#ifdef IUP
  /* iup initialization */
  IupOpen();
  #ifndef NO_CPICONTROLS
  IupGLCanvasOpen();
  IupControlsOpen();
  #endif
  #ifdef SPEECH_SDK
  IupSpeechOpen();
  #endif
#endif
  factoryCache = luaL_newstate();
  lua_newtable(factoryCache);
  lua_setglobal(factoryCache,"factories");
}

static tLuaCOMClassFactory* factoryCache_GetFactory(const char* luaclsid) {
  CriticalSectionObject cs(&cs_factoryCache); // won't let other threads enter till it goes out of scope
  lua_getglobal(factoryCache,"factories");
  lua_pushstring(factoryCache,luaclsid);
  lua_gettable(factoryCache,-2);
  tLuaCOMClassFactory* pFactory = (tLuaCOMClassFactory*)luaCompat_getPointer(factoryCache,-1);
  lua_pop(factoryCache,2);
  return pFactory;
}

static void factoryCache_PutFactory(const char* luaclsid, tLuaCOMClassFactory* pFactory) {
  CriticalSectionObject cs(&cs_factoryCache); // won't let other threads enter till it goes out of scope
  pFactory->AddRef();
  lua_getglobal(factoryCache,"factories");
  lua_pushstring(factoryCache,luaclsid);
  lua_pushlightuserdata(factoryCache,pFactory);
  lua_settable(factoryCache,-3);
  lua_pop(factoryCache,1);
}

static void factoryCache_Release() {
  CriticalSectionObject cs(&cs_factoryCache); // won't let other threads enter till it goes out of scope
  lua_getglobal(factoryCache,"factories");
    lua_pushnil(factoryCache); 
    while(lua_next(factoryCache,-2) != 0) {
    tLuaCOMClassFactory* pFactory = (tLuaCOMClassFactory*)luaCompat_getPointer(factoryCache,-1);
    pFactory->Release();
    lua_pop(factoryCache,1);
    }
  lua_pop(factoryCache,1);
  #ifdef IUP
    IupClose();
  #endif
}

static lua_State* luacom_DoRegistryFile(const char* luaclsid) {
  lua_State* L_inproc = NULL;
  char* fileName;
  char* key = new char[strlen(luaclsid)+18];

  strcpy(key, "CLSID\\");
  strcat(key,luaclsid);
  strcat(key,"\\ScriptFile");

  if(tCOMUtil::GetRegKeyValue(key,&fileName)) {
    L_inproc = luaL_newstate();
    luaL_openlibs(L_inproc);
    #ifdef IUP
      /* iuplua initialization */
      iuplua_open(L_inproc);
      iupkey_open(L_inproc);
      cdlua5_open(L_inproc);
      #ifndef NO_CPICONTROLS
      controlslua_open(L_inproc);
      gllua_open(L_inproc);
      #endif
      #ifdef SPEECH_SDK
      speechlua_open(L_inproc);
      #endif
      #ifdef USE_GLLUA
      gl_open(L_inproc);
      #endif
      iupluaim_open(L_inproc);
    #endif

    luacom_open(L_inproc);
    lua_pushvalue(L_inproc, LUA_REGISTRYINDEX);
    lua_pushstring(L_inproc,"inproc");
    lua_pushboolean(L_inproc,TRUE);
    lua_settable(L_inproc,-3);
    lua_pop(L_inproc,1);

    if(!lua_dofile(L_inproc,fileName)) {
      lua_pushstring(L_inproc,"StartAutomation");
      lua_gettable(L_inproc,-2);
      if(luaCompat_call(L_inproc, 0, 0)) {
        luacom_close(L_inproc);
        lua_close(L_inproc);
        L_inproc = NULL;
      }
    } else {
      luacom_close(L_inproc);
      lua_close(L_inproc);
      L_inproc = NULL;
    }
  }

  delete[] key;
  SAFEDELETEARR(fileName);
  return L_inproc;
}

static tLuaCOMClassFactory* luacom_GetInprocFactory(REFCLSID rclsid) {
  LPOLESTR clsid;
  try {
    CHK_COM_CODE(StringFromCLSID(rclsid, &clsid));
    BSTR clsid_bstr = SysAllocString(clsid);
    tStringBuffer luaclsid = tUtil::bstr2string(clsid_bstr);
    SysFreeString(clsid_bstr);
    CoTaskMemFree(clsid);
    tLuaCOMClassFactory* pFactory = factoryCache_GetFactory(luaclsid);
    if(pFactory == NULL) {
      lua_State* L_inproc = luacom_DoRegistryFile(luaclsid);
      if(L_inproc != NULL) {
        pFactory = new tLuaCOMClassFactory(L_inproc);
        factoryCache_PutFactory(luaclsid,pFactory);
      }
    }
    return pFactory;
   } catch(class tLuaCOMException&) {
      return NULL;
  }
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
  tLuaCOMClassFactory* pFactory = luacom_GetInprocFactory(rclsid);
  if(pFactory == NULL) return CLASS_E_CLASSNOTAVAILABLE;

  HRESULT hr = pFactory->QueryInterface(riid,ppv);

  if(FAILED(hr)) return hr;

  return S_OK;
}

extern "C"
BOOL WINAPI DllMain( HANDLE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
           )
{
  g_hInstance = (HINSTANCE)hModule;
  if(ul_reason_for_call == DLL_PROCESS_ATTACH) {
    factoryCache_Init();
  } else if(ul_reason_for_call == DLL_PROCESS_DETACH) {
    factoryCache_Release();
  }
    return TRUE;
}
