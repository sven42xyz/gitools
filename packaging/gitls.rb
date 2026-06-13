class Gitls < Formula
  desc "A fast, minimal tool to inspect and act on multiple git repositories"
  homepage "https://github.com/sven42xyz/gitools"
  url "https://github.com/sven42xyz/gitools/archive/refs/tags/v0.4.0.tar.gz"
  sha256 "TBD_AFTER_TAG"  # update with: shasum -a 256 of the v0.4.0 tarball
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
    assert_path_exists man1/"gitls.1"
  end
end
