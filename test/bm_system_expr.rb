ok = system("echo hello_from_expr")
puts ok
puts($? == 0)
ok = system("false")
puts ok
puts($? == 0)
