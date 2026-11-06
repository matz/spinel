# Each END block starts from the exception ending the program: a rescue in one
# ends the reading for that block and not for the blocks after it.
END { p [:first_registered, $!] }
END { begin; raise "inner"; rescue; end; p [:middle, $!] }
END { begin; raise "inner2"; rescue; end; p [:last_registered, $!] }
raise ArgumentError, "boom"
