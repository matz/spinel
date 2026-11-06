# An END block reads the exception ending the program as $!, as a hook does.
END { puts "#{$!.class} #{$!.message}" }
raise IOError, "disk"
