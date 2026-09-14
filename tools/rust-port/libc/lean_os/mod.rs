pub mod constants;

pub use self::constants::*;

use crate::prelude::*;

pub type ino_t = u64;
pub type off_t = i64;
pub type mode_t = u32;
pub type nlink_t = u32;
pub type dev_t = u64;
pub type blksize_t = u64;
pub type blkcnt_t = u64;
pub type time_t = i64;
pub type suseconds_t = i64;
pub type clock_t = i64;
pub type clockid_t = c_int;
pub type socklen_t = c_uint;
pub type sa_family_t = u16;
pub type sigset_t = c_uint;
pub type nfds_t = c_uint;
pub type rlim_t = u64;
pub type fsblkcnt_t = u64;
pub type fsfilcnt_t = u64;
pub type tcflag_t = c_uint;
pub type speed_t = c_uint;
pub type eventfd_t = u64;
pub type useconds_t = c_uint;
pub type id_t = c_uint;
pub type key_t = c_int;
pub type nl_item = c_int;
pub type pthread_t = c_int;
pub type pthread_key_t = c_int;
pub type wchar_t = i32;

extern_ty! {
    pub type sem_t;
    pub type Dl_info;
}

s! {
    pub struct stat {
        pub st_mode: crate::mode_t,
        pub st_size: crate::off_t,
        pub st_mtim: crate::timespec,
        pub st_atim: crate::timespec,
        pub st_ctim: crate::timespec,
        pub st_nlink: crate::nlink_t,
        pub st_ino: crate::ino_t,
        pub st_dev: crate::dev_t,
        pub st_rdev: crate::dev_t,
        pub st_uid: crate::uid_t,
        pub st_gid: crate::gid_t,
        pub st_blksize: crate::blksize_t,
        pub st_blocks: crate::blkcnt_t,
    }

    pub struct dirent {
        pub d_ino: crate::ino_t,
        pub d_type: c_uchar,
        pub d_name: [c_char; 256],
    }

    pub struct sockaddr {
        pub sa_family: crate::sa_family_t,
        pub sa_data: [c_char; 14],
    }

    pub struct in_addr {
        pub s_addr: crate::in_addr_t,
    }

    pub struct sockaddr_in {
        pub sin_family: crate::sa_family_t,
        pub sin_port: crate::in_port_t,
        pub sin_addr: crate::in_addr,
        pub sin_zero: [c_char; 8],
    }

    pub struct ip_mreq {
        pub imr_multiaddr: crate::in_addr,
        pub imr_interface: crate::in_addr,
    }

    pub struct sockaddr_in6 {
        pub sin6_family: crate::sa_family_t,
        pub sin6_port: crate::in_port_t,
        pub sin6_flowinfo: u32,
        pub sin6_addr: crate::in6_addr,
        pub sin6_scope_id: u32,
    }

    pub struct sockaddr_un {
        pub sun_family: crate::sa_family_t,
        pub sun_path: [c_char; 108],
    }

    pub struct msghdr {
        pub msg_name: *mut c_void,
        pub msg_namelen: crate::socklen_t,
        pub msg_iov: *mut crate::iovec,
        pub msg_iovlen: c_int,
        pub msg_control: *mut c_void,
        pub msg_controllen: crate::socklen_t,
        pub msg_flags: c_int,
    }

    pub struct cmsghdr {
        pub cmsg_len: size_t,
        pub cmsg_level: c_int,
        pub cmsg_type: c_int,
    }

    pub struct addrinfo {
        pub ai_flags: c_int,
        pub ai_family: c_int,
        pub ai_socktype: c_int,
        pub ai_protocol: c_int,
        pub ai_addrlen: crate::socklen_t,
        pub ai_addr: *mut crate::sockaddr,
        pub ai_canonname: *mut c_char,
        pub ai_next: *mut crate::addrinfo,
    }

    pub struct lconv {
        pub decimal_point: *mut c_char,
        pub thousands_sep: *mut c_char,
        pub grouping: *mut c_char,
        pub int_curr_symbol: *mut c_char,
        pub currency_symbol: *mut c_char,
        pub mon_decimal_point: *mut c_char,
        pub mon_thousands_sep: *mut c_char,
        pub mon_grouping: *mut c_char,
        pub positive_sign: *mut c_char,
        pub negative_sign: *mut c_char,
        pub int_frac_digits: c_char,
        pub frac_digits: c_char,
        pub p_cs_precedes: c_char,
        pub p_sep_by_space: c_char,
        pub n_cs_precedes: c_char,
        pub n_sep_by_space: c_char,
        pub p_sign_posn: c_char,
        pub n_sign_posn: c_char,
        pub int_p_cs_precedes: c_char,
        pub int_n_cs_precedes: c_char,
        pub int_p_sep_by_space: c_char,
        pub int_n_sep_by_space: c_char,
        pub int_p_sign_posn: c_char,
        pub int_n_sign_posn: c_char,
    }

    pub struct passwd {
        pub pw_name: *mut c_char,
        pub pw_passwd: *mut c_char,
        pub pw_uid: crate::uid_t,
        pub pw_gid: crate::gid_t,
        pub pw_gecos: *mut c_char,
        pub pw_dir: *mut c_char,
        pub pw_shell: *mut c_char,
    }

    pub struct sched_param {
        pub sched_priority: c_int,
    }

    pub struct pthread_attr_t {
        pub stack_size: size_t,
    }

    pub struct pthread_mutex_t {
        state: c_uint,
        type_: c_uint,
        owner: c_int,
        count: c_uint,
    }

    pub struct pthread_cond_t {
        seq: c_uint,
    }

    pub struct pthread_rwlock_t {
        state: c_uint,
    }

    pub struct pthread_mutexattr_t {
        type_: c_int,
    }

    pub struct pthread_condattr_t {
        unused: c_int,
    }

    pub struct pthread_rwlockattr_t {
        unused: c_int,
    }

    pub struct sigaction {
        pub sa_sigaction: crate::sighandler_t,
        pub sa_mask: crate::sigset_t,
        pub sa_flags: c_int,
    }

    pub struct siginfo_t {
        pub si_signo: c_int,
        pub si_code: c_int,
        pub si_errno: c_int,
        pub si_pid: crate::pid_t,
        pub si_uid: crate::uid_t,
        pub si_addr: *mut c_void,
        pub si_status: c_int,
        pub si_band: c_long,
        pub si_value: crate::sigval,
    }

    pub struct termios {
        pub c_iflag: crate::tcflag_t,
        pub c_oflag: crate::tcflag_t,
        pub c_cflag: crate::tcflag_t,
        pub c_lflag: crate::tcflag_t,
        pub c_cc: [crate::cc_t; 12],
    }

    pub struct flock {
        pub l_type: c_short,
        pub l_whence: c_short,
        pub l_start: crate::off_t,
        pub l_len: crate::off_t,
        pub l_pid: crate::pid_t,
    }

    pub struct statvfs {
        pub f_bsize: c_ulong,
        pub f_frsize: c_ulong,
        pub f_blocks: crate::fsblkcnt_t,
        pub f_bfree: crate::fsblkcnt_t,
        pub f_bavail: crate::fsblkcnt_t,
        pub f_files: crate::fsfilcnt_t,
        pub f_ffree: crate::fsfilcnt_t,
        pub f_favail: crate::fsfilcnt_t,
        pub f_fsid: c_ulong,
        pub f_flag: c_ulong,
        pub f_namemax: c_ulong,
    }

    pub struct utsname {
        pub sysname: [c_char; 65],
        pub nodename: [c_char; 65],
        pub release: [c_char; 65],
        pub version: [c_char; 65],
        pub machine: [c_char; 65],
        pub domainname: [c_char; 65],
    }

    pub struct itimerspec {
        pub it_interval: crate::timespec,
        pub it_value: crate::timespec,
    }

    pub struct fd_set {
        fds_bits: [c_ulong; 2],
    }

    pub struct tm {
        pub tm_sec: c_int,
        pub tm_min: c_int,
        pub tm_hour: c_int,
        pub tm_mday: c_int,
        pub tm_mon: c_int,
        pub tm_year: c_int,
        pub tm_wday: c_int,
        pub tm_yday: c_int,
        pub tm_isdst: c_int,
        pub tm_gmtoff: c_long,
        pub tm_zone: *const c_char,
    }

    pub struct sockaddr_storage {
        pub ss_family: crate::sa_family_t,
        __pad: [c_char; 126],
    }

    pub struct epoll_event {
        pub events: u32,
        __reserved: u32,
        pub u64: u64,
    }
}

pub const PTHREAD_MUTEX_INITIALIZER: pthread_mutex_t = pthread_mutex_t {
    state: 0,
    type_: 0,
    owner: 0,
    count: 0,
};
pub const PTHREAD_COND_INITIALIZER: pthread_cond_t = pthread_cond_t { seq: 0 };
pub const PTHREAD_RWLOCK_INITIALIZER: pthread_rwlock_t = pthread_rwlock_t { state: 0 };
pub const PTHREAD_MUTEX_NORMAL: c_int = 0;
pub const PTHREAD_MUTEX_DEFAULT: c_int = 0;
pub const PTHREAD_MUTEX_RECURSIVE: c_int = 1;
pub const PTHREAD_MUTEX_ERRORCHECK: c_int = 2;
pub const PTHREAD_STACK_MIN: size_t = 16 * 1024;
pub const PTHREAD_KEYS_MAX: c_int = 32;
pub const MAP_FAILED: *mut c_void = !0 as *mut c_void;

f! {
    pub const unsafe fn CMSG_ALIGN(len: size_t) -> size_t {
        (len + size_of::<size_t>() - 1) & !(size_of::<size_t>() - 1)
    }

    pub const unsafe fn CMSG_SPACE(length: c_uint) -> c_uint {
        (CMSG_ALIGN(length as size_t) + CMSG_ALIGN(size_of::<cmsghdr>())) as c_uint
    }

    pub const unsafe fn CMSG_LEN(length: c_uint) -> c_uint {
        (CMSG_ALIGN(size_of::<cmsghdr>()) + length as size_t) as c_uint
    }

    pub unsafe fn CMSG_DATA(cmsg: *const cmsghdr) -> *mut c_uchar {
        (cmsg as *mut c_uchar).offset(CMSG_ALIGN(size_of::<cmsghdr>()) as isize)
    }

    pub unsafe fn CMSG_FIRSTHDR(mhdr: *const msghdr) -> *mut cmsghdr {
        if (*mhdr).msg_controllen as size_t >= size_of::<cmsghdr>() {
            (*mhdr).msg_control as *mut cmsghdr
        } else {
            core::ptr::null_mut::<cmsghdr>()
        }
    }

    pub unsafe fn CMSG_NXTHDR(mhdr: *const msghdr, cmsg: *const cmsghdr) -> *mut cmsghdr {
        if cmsg.is_null() || (*cmsg).cmsg_len < size_of::<cmsghdr>() {
            return core::ptr::null_mut::<cmsghdr>();
        }
        let next = (cmsg as usize + CMSG_ALIGN((*cmsg).cmsg_len)) as *mut cmsghdr;
        let end = (*mhdr).msg_control as usize + (*mhdr).msg_controllen as usize;
        if next as usize + size_of::<cmsghdr>() > end {
            core::ptr::null_mut::<cmsghdr>()
        } else {
            next
        }
    }

    pub unsafe fn FD_ZERO(set: *mut fd_set) -> () {
        for slot in (*set).fds_bits.iter_mut() {
            *slot = 0;
        }
    }

    pub unsafe fn FD_SET(fd: c_int, set: *mut fd_set) -> () {
        let bits = 8 * size_of::<c_ulong>();
        (*set).fds_bits[fd as usize / bits] |= 1 << (fd as usize % bits);
    }

    pub unsafe fn FD_CLR(fd: c_int, set: *mut fd_set) -> () {
        let bits = 8 * size_of::<c_ulong>();
        (*set).fds_bits[fd as usize / bits] &= !(1 << (fd as usize % bits));
    }

    pub unsafe fn FD_ISSET(fd: c_int, set: *const fd_set) -> bool {
        let bits = 8 * size_of::<c_ulong>();
        ((*set).fds_bits[fd as usize / bits] & (1 << (fd as usize % bits))) != 0
    }
}

safe_f! {
    pub const safe fn WIFEXITED(status: c_int) -> bool {
        (status & 0x7f) == 0
    }

    pub const safe fn WEXITSTATUS(status: c_int) -> c_int {
        (status >> 8) & 0xff
    }

    pub const safe fn WIFSTOPPED(status: c_int) -> bool {
        (status & 0xff) == 0x7f
    }

    pub const safe fn WSTOPSIG(status: c_int) -> c_int {
        (status >> 8) & 0xff
    }

    pub const safe fn WIFSIGNALED(status: c_int) -> bool {
        (status & 0x7f) != 0 && (status & 0xff) != 0x7f
    }

    pub const safe fn WTERMSIG(status: c_int) -> c_int {
        status & 0x7f
    }

    pub const safe fn WCOREDUMP(_status: c_int) -> bool {
        false
    }

    pub const safe fn WIFCONTINUED(status: c_int) -> bool {
        status == 0xffff
    }
}

extern "C" {
    pub fn __errno_location() -> *mut c_int;

    pub fn memfd_create(name: *const c_char, flags: c_uint) -> c_int;
    pub fn memfd_add_seals(fd: c_int, seals: c_uint) -> c_int;
    pub fn memfd_seals(fd: c_int) -> c_int;

    pub fn eventfd(initval: c_uint, flags: c_int) -> c_int;
    pub fn eventfd_read(fd: c_int, value: *mut eventfd_t) -> c_int;
    pub fn eventfd_write(fd: c_int, value: eventfd_t) -> c_int;

    pub fn epoll_create(size: c_int) -> c_int;
    pub fn epoll_create1(flags: c_int) -> c_int;
    pub fn epoll_ctl(epfd: c_int, op: c_int, fd: c_int, event: *mut epoll_event) -> c_int;
    pub fn epoll_wait(
        epfd: c_int,
        events: *mut epoll_event,
        maxevents: c_int,
        timeout: c_int,
    ) -> c_int;

    pub fn timerfd_create(clockid: crate::clockid_t, flags: c_int) -> c_int;
    pub fn timerfd_settime(
        fd: c_int,
        flags: c_int,
        new_value: *const itimerspec,
        old_value: *mut itimerspec,
    ) -> c_int;
    pub fn timerfd_gettime(fd: c_int, current: *mut itimerspec) -> c_int;

    pub fn accept4(
        fd: c_int,
        addr: *mut crate::sockaddr,
        len: *mut crate::socklen_t,
        flags: c_int,
    ) -> c_int;

    pub fn uname(buf: *mut utsname) -> c_int;

    pub fn bind(fd: c_int, addr: *const crate::sockaddr, len: crate::socklen_t) -> c_int;
    pub fn recvfrom(
        fd: c_int,
        buf: *mut c_void,
        len: size_t,
        flags: c_int,
        addr: *mut crate::sockaddr,
        addrlen: *mut crate::socklen_t,
    ) -> ssize_t;
    pub fn sendto(
        fd: c_int,
        buf: *const c_void,
        len: size_t,
        flags: c_int,
        addr: *const crate::sockaddr,
        addrlen: crate::socklen_t,
    ) -> ssize_t;

    pub fn ioctl(fd: c_int, request: c_ulong, ...) -> c_int;
    pub fn readv(fd: c_int, iov: *const crate::iovec, count: c_int) -> ssize_t;
    pub fn writev(fd: c_int, iov: *const crate::iovec, count: c_int) -> ssize_t;
    pub fn dirfd(dirp: *mut crate::DIR) -> c_int;

    pub fn clock_gettime(clock: crate::clockid_t, tp: *mut crate::timespec) -> c_int;
    pub fn clock_getres(clock: crate::clockid_t, res: *mut crate::timespec) -> c_int;
    pub fn utimensat(
        dirfd: c_int,
        path: *const c_char,
        times: *const crate::timespec,
        flags: c_int,
    ) -> c_int;
    pub fn futimens(fd: c_int, times: *const crate::timespec) -> c_int;

    pub fn getpwuid_r(
        uid: crate::uid_t,
        pwd: *mut passwd,
        buf: *mut c_char,
        buflen: size_t,
        result: *mut *mut passwd,
    ) -> c_int;
    pub fn setgroups(ngroups: size_t, ptr: *const crate::gid_t) -> c_int;

    pub fn pthread_create(
        thread: *mut crate::pthread_t,
        attr: *const crate::pthread_attr_t,
        start: extern "C" fn(*mut c_void) -> *mut c_void,
        arg: *mut c_void,
    ) -> c_int;

    pub fn getrandom(buf: *mut c_void, buflen: size_t, flags: c_uint) -> ssize_t;

    pub fn sched_getaffinity(
        pid: crate::pid_t,
        cpusetsize: size_t,
        cpuset: *mut c_void,
    ) -> c_int;

    pub fn copy_file_range(
        fd_in: c_int,
        off_in: *mut off_t,
        fd_out: c_int,
        off_out: *mut off_t,
        len: size_t,
        flags: c_uint,
    ) -> ssize_t;
}
