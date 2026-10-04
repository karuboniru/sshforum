Name:           sshforum
Version:        0.1.0
Release:        3%{?dist}
Summary:        Anonymous SSH terminal forum
License:        MIT
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  cmake-rpm-macros
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  pkgconfig(libssh) >= 0.10
BuildRequires:  pkgconfig(libcrypto) >= 3.0
BuildRequires:  pkgconfig(sqlite3)
BuildRequires:  python3-paramiko
BuildRequires:  systemd-rpm-macros
Requires:       systemd >= 252
%{?systemd_requires}

%description
sshforum is an anonymous forum served through an SSH terminal interface.
It stores posts and replies in SQLite and creates its SSH host key on first
start.

%prep
%autosetup

%build
%cmake \
    -DSSHFORUM_INSTALL_SYSTEMD=ON \
    -DSSHFORUM_SYSTEMD_UNIT_DIR=%{_unitdir} \
    -DBUILD_TESTING=ON \
    -DPython3_EXECUTABLE=/usr/bin/python3
%cmake_build

%install
%cmake_install

%check
ctest --test-dir %{_vpath_builddir} --output-on-failure

%post
%systemd_post sshforum.service

%preun
%systemd_preun sshforum.service

%postun
%systemd_postun_with_restart sshforum.service

%files
%license LICENSE
%doc README.md docs/packaging.md scripts/test-systemd.sh
%{_bindir}/sshforum
%{_unitdir}/sshforum.service

%changelog
* Sun Oct 04 2026 sshforum contributors - 0.1.0-3
- Add persistent pseudonymous author identities and automatic SQLite migration

* Sun Oct 04 2026 sshforum contributors - 0.1.0-2
- Remove fragile configuration-file bind mounts under SELinux
- Add UTF-8 cursor movement, insertion and deletion to post editors

* Sun Oct 04 2026 sshforum contributors - 0.1.0-1
- Initial RPM package
