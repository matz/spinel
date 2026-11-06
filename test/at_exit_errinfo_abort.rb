# `abort` reads as a SystemExit with its message and status 1.
at_exit { puts "#{$!.class} #{$!.status} #{$!.message}" }
abort "giving up"
