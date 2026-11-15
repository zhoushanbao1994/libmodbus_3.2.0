EXTRA_DIST = README.md unit-tests.sh proxy-tests.sh LICENSE

noinst_PROGRAMS = \
	bandwidth-server-one \
	bandwidth-server-many-up \
	bandwidth-client \
	random-test-server \
	random-test-client \
	unit-test-server \
	unit-test-client \
	proxy-test-server \
	proxy-test-client \
	version

common_ldflags = \
	$(top_builddir)/src/libmodbus.la

bandwidth_server_one_SOURCES = bandwidth-server-one.c
bandwidth_server_one_LDADD = $(common_ldflags)

bandwidth_server_many_up_SOURCES = bandwidth-server-many-up.c
bandwidth_server_many_up_LDADD = $(common_ldflags)

bandwidth_client_SOURCES = bandwidth-client.c
bandwidth_client_LDADD = $(common_ldflags)

random_test_server_SOURCES = random-test-server.c
random_test_server_LDADD = $(common_ldflags)

random_test_client_SOURCES = random-test-client.c
random_test_client_LDADD = $(common_ldflags)

unit_test_server_SOURCES = unit-test-server.c unit-test.h
unit_test_server_LDADD = $(common_ldflags)

unit_test_client_SOURCES = unit-test-client.c unit-test.h
unit_test_client_LDADD = $(common_ldflags)

proxy_test_server_SOURCES = proxy-test-server.c
proxy_test_server_LDADD = $(common_ldflags)

proxy_test_client_SOURCES = proxy-test-client.c unit-test.h
proxy_test_client_LDADD = $(common_ldflags)

version_SOURCES = version.c
version_LDADD = $(common_ldflags)

AM_CPPFLAGS = \
    -include $(top_builddir)/config.h \
    -DSYSCONFDIR=\""$(sysconfdir)"\" \
    -DLIBEXECDIR=\""$(libexecdir)"\" \
    -I${top_srcdir}/src \
    -I${top_builddir}/src

AM_CFLAGS = $(LIBMODBUSCFLAGS) $(WARNING_CFLAGS)

CLEANFILES = *~ *.log

noinst_SCRIPTS=unit-tests.sh
TESTS=./unit-tests.sh

# These integration tests use POSIX descriptors for both RTU and TCP backends.
if !OS_WIN32
noinst_PROGRAMS += frame-callback-test
frame_callback_test_SOURCES = frame-callback-test.c
frame_callback_test_LDADD = $(common_ldflags)
noinst_PROGRAMS += frame-callback-selftest
frame_callback_selftest_SOURCES = frame-callback-selftest.c
frame_callback_selftest_LDADD = $(common_ldflags)
# Linux 动态加载器把 ORIGIN 展开为 ELF 自身所在目录，不依赖启动工作目录。
# 双美元经过 make 后保留一个美元，单引号阻止 shell 提前展开 ORIGIN。
if OS_LINUX
frame_callback_test_LDFLAGS = -Wl,-rpath,'$$ORIGIN/lib'
frame_callback_selftest_LDFLAGS = -Wl,-rpath,'$$ORIGIN/lib'
endif
# 直接由 Automake 调用 Python，不再保留仅转发命令的 shell 包装文件。
TEST_EXTENSIONS = .py
PY_LOG_COMPILER = python3
TESTS += frame-callback-selftest frame-callback-cli-tests.py
EXTRA_DIST += frame-callback-cli-tests.py
endif
