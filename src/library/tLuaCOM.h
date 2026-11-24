/*
 * ttLuaCOM.h
 */


#ifndef __LUACOM_H
#define __LUACOM_H

#include <ole2.h>
#include <ocidl.h> 
#include <vector>
#include <list>

extern "C"
{
#include <lua.h>
#include <lauxlib.h>
}

#include "luabeans.h"
#include "tLuaCOMTypeHandler.h"

#include "tLuaObjList.h"

enum tWhichInterface {DISP, SOURCE};

// Types of FUNCDESC

struct FuncInfo
{
  char*     name;

  FUNCDESC* propget;
  ITypeInfo* propget_owner;
  FUNCDESC* propput;
  ITypeInfo* propput_owner;
  FUNCDESC* func;
  ITypeInfo* func_owner;
};


class tLuaCOM
{
public:
  bool hasTypeInfo(void);
  int Lock(void);
  int Unlock(void);
  ITypeInfo* GetTypeInfo(void);
  CLSID GetCLSID(void);
  void GetIID(IID *piid);
  IDispatch * GetIDispatch(void);
  void ReleaseFuncDesc(ITypeInfo *owner, FUNCDESC *pfuncdesc);
  ITypeInfo * GetDefaultEventsInterface(void);
  const char* GetObjName() const { return objName; }
  static tLuaCOM * CreateLuaCOM(
    lua_State* L,
    IDispatch * pdisp,
    const CLSID& coclass = IID_NULL,
    ITypeInfo* typeinfo=NULL,
    bool untyped = false,
    const char* name=NULL
    );

  void getHelpInfo(char **ppHelpFile, DWORD *pHelpContext);
  bool isMember(const char *name);

   ~tLuaCOM();

   DWORD addConnection(tLuaCOM *client);
   void releaseConnection(tLuaCOM* server, DWORD cookie);
   void releaseConnection();
   HRESULT releaseConnections(void);

   int call(
     lua_State* L, 
     DISPID dispid,
     int invkind,
     FUNCDESC *pFuncDesc,
     tLuaObjList params);

   bool getFUNCDESC(const char* name, FuncInfo& funcinfo);
   bool getConstant(lua_State* L, const char* name);
   bool getDISPID(const char* name, DISPID* dispid);
   void releaseComObject();
   void setConversion(int dates, int decimals)
   {
     typehandler->setConversion(dates, decimals);
   }

  volatile static long NEXT_ID;
protected:
  tCOMPtr<ITypeComp> ptcomp;
  tCOMPtr<ITypeComp> plib_tcomp;
  long lock_count;
  CLSID clsid;
  struct Connection
  {
    Connection(IConnectionPoint *point, const IID& interface_id)
      : point(point), interface_id(interface_id), cookie(0) {}

    tCOMPtr<IConnectionPoint> point;
    IID interface_id;
    DWORD cookie;
  };
  typedef std::list<Connection> ConnectionList;
  ConnectionList connections;
  IID last_connection_interface;
  DWORD last_connection_cookie;

  tLuaCOM(
    lua_State* L,
    IDispatch *pdisp_arg,
    ITypeInfo *ptinfo_arg,
    CLSID coclass
    );

  tLuaCOMTypeHandler * typehandler;


  tCOMPtr<IDispatch> pdisp;
  tCOMPtr<ITypeInfo> ptinfo;

  // struct to hold all type information for a
  // member

  std::vector<FuncInfo> mFuncInfo;
private:
  struct CallContext;
  static int protectedCall(lua_State* L);
  int callWithDispatch(lua_State* L, CallContext& context);
  void checkComObject() const;
  long ID;
  char* objName;
};

#endif // __LUACOM_H
