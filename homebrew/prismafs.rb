class Prismafs < Formula
  desc "A lightweight, layered userspace filesystem inspired by Plan 9"
  homepage "http://ithas-site.com"
  url "https://github.com/goranb131/ITHAS-prismaFS/releases/download/v1.0.0/prismafs-1.0.0.tar.gz"
  sha256 "02ca028430f75e8746e3c7e476b3c4368d3c7752bd002318d11ac7d634d442b8"
  license "Apache-2.0"

  def install
    system "make", "CC=clang", "install", "INSTALL_DIR=#{prefix}/bin", "MAN_DIR=#{man1}"
  end

  test do
    # Check if the binary exists
    assert_predicate bin/"prismafs", :exist?, "prismafs binary not installed"
    # Ensure it runs correctly
    system "#{bin}/prismafs", "--version"
  end
end