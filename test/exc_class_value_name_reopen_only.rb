# e.class.name when only reopened builtin exception classes define
# self.name, and a program exception class with its own self.name that no
# code names or raises
class Unused < StandardError; def self.name = "UNUSED"; end
class IOError; def self.name = "IOE"; end
class RuntimeError; def self.name = "RTE"; end
e = (raise "x" rescue $!)
p e.class.name
z = (1 / 0 rescue $!)
p z.class.name
i = (raise IOError rescue $!)
p i.class.name
begin; raise Exception; rescue Exception => x; end
p x.class.name
p "#{e.class.name}/#{z.class.name}"
