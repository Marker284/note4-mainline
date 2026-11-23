/* stub libhardware_legacy: kernel wakelocks instead of ISystemSuspend/hwbinder */
int open(const char*, int, ...); long write(int, const void*, unsigned long); int close(int);
unsigned int strlen(const char*);
static int wl(const char *f, const char *id){ int fd=open(f,01|02000000); if(fd<0) return -1;
  long r=write(fd,id,strlen(id)); close(fd); return r<0?-1:0; }
int acquire_wake_lock(int lock, const char *id){ (void)lock; return wl("/sys/power/wake_lock", id); }
int release_wake_lock(const char *id){ return wl("/sys/power/wake_unlock", id); }
