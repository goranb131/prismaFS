class Prismafs < Formula
  desc "A lightweight, layered userspace filesystem inspired by Plan 9"
  homepage "http://ithas-site.com"
  url "https://github.com/goranb131/ITHAS-prismaFS/archive/v1.0.0.tar.gz"
  sha256 "4a1e89d4dbd49b0e8d6d1374911985d2b5e53ab15f55d35d26a098caa473ebd6"
  license "Apache-2.0"

  depends_on "clang" => :build
  depends_on "macfuse"

  def install
    system "make", "CC=clang"
    bin.install "prismafs"
  end

  test do
    # Check if the binary exists
    assert_predicate bin/"prismafs", :exist?, "prismafs binary not installed"
    # Ensure it runs correctly
    system "#{bin}/prismafs", "--version"
  end
end
