# spinel: gc-stress
# Process::Status#== compares the status word: with another status (`$? ==
# $?`, `Process.last_status == $?`), an Integer or a Float; any other value
# is unequal, and nil ($? before any child) equals only nil. Two statuses
# were refused as an unsupported equality.
p $? == nil, $? == $?
system("exit 3")
p $? == $?
p Process.last_status == $?
a = $?
system("exit 3")
p a == $?, a.equal?($?), a.equal?(a)
system("true")
p a == $?, a != $?
p a == 768, a == 768.0, a != 768, a == "x", a != "x", a == 2.5
