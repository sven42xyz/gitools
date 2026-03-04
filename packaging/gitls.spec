Name:           gitls
Version:        0.3.0
Release:        1%{?dist}
Summary:        A fast, minimal tool to inspect and act on multiple git repositories

License:        MIT
URL:            https://github.com/sven42xyz/gitools
Source0:        https://github.com/sven42xyz/gitools/archive/refs/tags/v%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  libgit2-devel
Requires:       libgit2

%description
gitls recursively scans a directory for git repositories and displays
their status in a compact table: branch name, staged/modified/untracked
file counts, ahead/behind remote, and relative last-commit time.
It also supports bulk branch switching with the -s flag.

%prep
%autosetup

%build
%make_build PREFIX=%{_prefix}

%install
%make_install PREFIX=%{_prefix}

%files
%license LICENSE
%doc README.md NOTICE
%{_bindir}/gitls

%changelog
* Tue Mar 04 2026 Sven Siepermann <sven@siepermann.dev> - 0.3.0-1
- fetch and pull subcommands for bulk network operations
- ~/.gitlsrc config file with default_dir, max_depth, skip_dirs, no_color
- GITLS_CONFIG env var to override config path
- Two-phase parallel processing (local queries threaded, network sequential)
- Fix: gitls . respects explicit path over default_dir

* Mon Feb 24 2025 Sven Siepermann <sven@siepermann.dev> - 0.1.0-1
- Initial package
