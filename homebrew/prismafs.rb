class Prismafs < Formula
  desc "A lightweight, layered userspace filesystem inspired by Plan 9"
  homepage "http://ithas-site.com"
  url "https://github.com/goranb131/ITHAS-prismaFS/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "5b8e44706496dbebd696d07da7938570f54aba004c1ef894ba99f03dbbebb367"
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