# trap returns "DEFAULT" as the previous handler of a signal Ruby handles
# itself (INT, HUP, QUIT, TERM, ALRM, USR1, USR2, CHLD), nil for PIPE and
# "SYSTEM_DEFAULT" for the others, before a trap and after trap(sig, "DEFAULT").
%w[INT HUP QUIT TERM ALRM USR1 USR2 CHLD WINCH TSTP URG PIPE].each do |s|
  first = trap(s) { }
  trap(s, "DEFAULT")
  puts "#{s} #{first.inspect} #{trap(s, "DEFAULT").inspect}"
end
