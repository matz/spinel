# END blocks and at_exit hooks share one stack: each END registers when it
# runs, the first time only, and the stack runs last in first out at exit.
# Spinel registered every top-level END at program start, after the at_exit
# hooks had run, and an END in a block or a loop not at all.
x = 1
END { puts "END1 x=#{x}" }
at_exit { puts "at_exit" }
END { puts "END2"; END { puts "nested END" } }
i = 0
while i < 3
  END { puts "loop END, registered once, i=#{i}" }
  i += 1
end
[1, 2].each { |v| END { puts "block END v=#{v}" } }
if i > 10
  END { puts "never registered" }
end
x = 2
puts "main"
exit 0
END { puts "after exit, never registered" }
