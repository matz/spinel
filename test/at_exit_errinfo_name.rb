# A NameError the runtime raises reads its name in a hook.
at_exit { puts "#{$!.class} #{$!.name.inspect}" }
foo_bar_undefined
