class Gitls < Formula
  desc "A fast, minimal tool to inspect and act on multiple git repositories"
  homepage "https://github.com/sven42xyz/gitools"
  url "https://github.com/sven42xyz/gitools/archive/refs/tags/v0.2.0.tar.gz"
  sha256 "985305bb8d7a0ba1b7a93fbff591663f449030f02238f45de7a21a8b3cfb2f5c"
  license "MIT"
  head "https://github.com/sven42xyz/gitools.git", branch: "main"

  depends_on "libgit2"

  def install
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
  end
end
