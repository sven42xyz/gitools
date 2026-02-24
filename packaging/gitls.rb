class Gitls < Formula
  desc "A fast, minimal tool to inspect and act on multiple git repositories"
  homepage "https://github.com/sven42xyz/gitools"
  url "https://github.com/sven42xyz/gitools/archive/refs/tags/v0.1.0.tar.gz"
  # sha256 is filled in after the release tarball exists:
  # sha256 "REPLACE_WITH_SHA256"
  license "MIT"
  head "https://github.com/sven42xyz/gitools.git", branch: "main"

  depends_on "libgit2"

  def install
    system "make", "CC=#{ENV.cc}",
                   "PREFIX=#{prefix}",
                   "install"
  end

  test do
    system "#{bin}/gitls", "--version"
    system "#{bin}/gitls", testpath.to_s
  end
end
