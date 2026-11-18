# A method the program makes under a computed name may be at_exit and run
# its block at once, so the block starts at its call and may run again.
name = "at_" + "exit"
Object.send(:define_method, name) { |&b| b.call; b }
at_exit { p :now }
p :after
