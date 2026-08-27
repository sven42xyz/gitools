class Gitls < Formula
  desc "A fast, minimal tool to inspect and act on multiple git repositories"
  homepage "https://github.com/sven42xyz/gitools"
  url "https://github.com/sven42xyz/gitools/archive/refs/tags/v0.5.1.tar.gz"
  sha256 "TBD_AFTER_TAG"  # update with: shasum -a 256 of the v0.5.1 tarball
  license "MIT"
  head "https://github.com/sven42xyz/gitools.git", branch: "main"

  depends_on "libgit2"

  def install
    # Homebrew's compiler shim appends -march=<cpu> (HOMEBREW_OPTFLAGS) to every
    # invocation. On installations where the detected CPU and the active
    # toolchain disagree, clang rejects that architecture name outright and the
    # build dies before it starts. gitls is I/O-bound and the Makefile already
    # compiles with -O2, so the CPU tuning buys nothing here.
    ENV["HOMEBREW_OPTFLAGS"] = ""

    libgit2 = Formula["libgit2"]
    system "make", "CC=#{ENV.cc}",
                   "PREFIX=#{prefix}",
                   "LIBGIT2_CFLAGS=-I#{libgit2.opt_include}",
                   "LIBGIT2_LIBS=-L#{libgit2.opt_lib} -lgit2",
                   "install"
  end

  test do
    system "#{bin}/gitls", "--version"
    system "#{bin}/gitls", testpath.to_s
    assert_path_exists man1/"gitls.1"
  end
end
