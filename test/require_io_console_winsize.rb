# strict-require: `require "io/console"` enables the IO console surface
# (IO#winsize). Without the require, winsize is unresolved under
# SPINEL_STRICT_REQUIRE (matching CRuby's NoMethodError); with it, the
# C-native sp_File_winsize is provided. The harness redirects stdout, so
# winsize raises Errno::ENOTTY, as CRuby's does. Locks the enable path
# (works with strict require on or off).
require "io/console"
begin
  p STDOUT.winsize
rescue Errno::ENOTTY => e
  p e.class
end

# A count CRuby refuses is its ArgumentError, before the ioctl, on a typed
# handle and on one read back out of a container.
begin
  STDOUT.winsize(1)
rescue ArgumentError => e
  p e.message
end
h = [STDOUT, nil][0]
begin
  h.winsize(1, 2)
rescue ArgumentError => e
  p e.message
end
