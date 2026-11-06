# RbConfig::CONFIG, as far as a compiled program can have it: the build
# configuration library code branches on (host_os, host_cpu, the shared
# library suffixes), describing the machine the program was compiled for,
# derived from RUBY_PLATFORM; and the installation directories library code
# compares paths against (a backtrace cleaner's `line.start_with?
# (CONFIG["rubylibdir"])`), under a conventional prefix, since the compiled
# program has no Ruby installation: Strings that match no path of its own.
# The parser splices this file when the program names `RbConfig` and
# defines none of its own.
module RbConfig
  __plat = RUBY_PLATFORM
  __darwin = __plat.include?("darwin")
  __cpu = __plat.split("-").first
  __cpu = "arm64" if __darwin && __cpu == "aarch64"
  __os = __darwin ? "darwin" : (__plat.include?("linux") ? "linux" : __plat.split("-").last)
  __ver = RUBY_VERSION.split(".")[0, 2].join(".") + ".0"
  CONFIG = {
    "host_os" => __os, "target_os" => __os, "build_os" => __os,
    "host_cpu" => __cpu, "target_cpu" => __cpu, "build_cpu" => __cpu,
    "arch" => "#{__cpu}-#{__os}", "ruby_version" => __ver,
    "MAJOR" => RUBY_VERSION.split(".")[0], "MINOR" => RUBY_VERSION.split(".")[1],
    "EXEEXT" => "", "DLEXT" => (__darwin ? "bundle" : "so"),
    "SOEXT" => (__darwin ? "dylib" : "so"), "LIBEXT" => "a",
    "host" => __plat, "target" => __plat, "ruby_install_name" => "ruby",
    "prefix" => "/usr/local", "bindir" => "/usr/local/bin", "libdir" => "/usr/local/lib",
    "rubylibprefix" => "/usr/local/lib/ruby",
    "rubylibdir" => "/usr/local/lib/ruby/#{__ver}",
    "rubyarchdir" => "/usr/local/lib/ruby/#{__ver}/#{__cpu}-#{__os}",
    "sitelibdir" => "/usr/local/lib/ruby/site_ruby/#{__ver}",
    "sitearchdir" => "/usr/local/lib/ruby/site_ruby/#{__ver}/#{__cpu}-#{__os}",
    "vendorlibdir" => "/usr/local/lib/ruby/vendor_ruby/#{__ver}",
    "vendorarchdir" => "/usr/local/lib/ruby/vendor_ruby/#{__ver}/#{__cpu}-#{__os}"
  }

  def self.ruby = "ruby"
end
