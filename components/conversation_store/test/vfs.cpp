#include "conversation_store.hpp"
#include "checked_vfs.hpp"
#include "sqlite3.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <sys/stat.h>
#include <string>
#include <unistd.h>
using namespace conversation_store;
int main(){
    char name[]="/tmp/arvel-vfs-XXXXXX";int seed=mkstemp(name);assert(seed>=0);close(seed);
    void *heap=malloc(kSqliteHeapBytes);assert(heap);assert(sqlite3_config(SQLITE_CONFIG_HEAP,heap,kSqliteHeapBytes,32)==SQLITE_OK);
    assert(internal::configure_vfs(name)==SQLITE_OK);assert(sqlite3_initialize()==SQLITE_OK);
    sqlite3_vfs *v=sqlite3_vfs_find("arvel-checked-fat");assert(v);std::vector<unsigned char> bytes(v->szOsFile);
    auto *f=reinterpret_cast<sqlite3_file *>(bytes.data());int flags=0;
    assert(v->xOpen(v,name,f,SQLITE_OPEN_MAIN_DB|SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,&flags)==SQLITE_OK);
    const auto *io=f->pMethods;assert(io->xWrite(f,"abcdef",6,0)==SQLITE_OK);assert(io->xSync(f,SQLITE_SYNC_FULL)==SQLITE_OK);
    char out[8];memset(out,0x55,sizeof(out));assert(io->xRead(f,out,8,0)==SQLITE_IOERR_SHORT_READ);assert(!memcmp(out,"abcdef\0\0",8));
    assert(io->xTruncate(f,3)==SQLITE_OK);sqlite3_int64 size=0;assert(io->xFileSize(f,&size)==SQLITE_OK&&size==3);
    struct stat st;assert(stat(name,&st)==0&&st.st_size==3); // Not a no-op truncate.
    internal::set_fault(int(Fault::truncate),0);assert(io->xTruncate(f,1)==SQLITE_IOERR_TRUNCATE);assert(io->xFileSize(f,&size)==SQLITE_OK&&size==3);
    internal::set_fault(int(Fault::short_write),0);assert(io->xWrite(f,"wxyz",4,3)==SQLITE_IOERR_WRITE);
    internal::set_fault(int(Fault::sync),0);assert(io->xSync(f,SQLITE_SYNC_FULL)==SQLITE_IOERR_FSYNC);assert(io->xSync(f,SQLITE_SYNC_FULL)==SQLITE_OK);
    internal::set_fault(int(Fault::full),0);assert(io->xWrite(f,"q",1,0)==SQLITE_FULL);
    internal::set_fault(int(Fault::short_read),0);memset(out,0x55,sizeof(out));assert(io->xRead(f,out,3,0)==SQLITE_IOERR_READ);assert(out[0]=='a'&&out[1]==0&&out[2]==0);
    const std::string wal=std::string(name)+"-wal";
    int present=-1;
    assert(v->xAccess(v,wal.c_str(),SQLITE_ACCESS_EXISTS,&present)==SQLITE_OK&&present==0);
    FILE *leftover=fopen(wal.c_str(),"wb");assert(leftover);assert(fwrite("x",1,1,leftover)==1);assert(fclose(leftover)==0);
    assert(v->xAccess(v,wal.c_str(),SQLITE_ACCESS_EXISTS,&present)==SQLITE_IOERR_ACCESS);
    assert(stat(wal.c_str(),&st)==0&&st.st_size==1); // Refusal never erases unsupported recovery data.
    assert(unlink(wal.c_str())==0);
    assert(io->xClose(f)==SQLITE_OK);assert(sqlite3_shutdown()==SQLITE_OK);free(heap);unlink(name);
    printf("checked FAT VFS: real truncate, short I/O, full and checked sync passed\n");
}
