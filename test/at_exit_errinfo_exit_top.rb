# `exit` at the top level, with no begin around it, is what a hook reads too.
at_exit { puts "#{$!.class} #{$!.success?} #{$!.inspect}" }
puts "body"
exit
