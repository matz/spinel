# e.class.name calls the class method `name` an exception class defines:
# a reopened builtin exception class's, a program exception class's own,
# or one a subclass inherits, the nearest up the class's ancestry
class StandardError; def self.name = "SE!"; end
class RuntimeError; def self.name = "RTE"; end
class MyErr < RuntimeError; end
class Own < MyErr; def self.name = "OWN"; end
class Other < StandardError; end
class Plain; def self.name = "PL"; end
e = (raise "x" rescue $!)
p e.class.name
p e.class.to_s
a = (raise ArgumentError rescue $!)
p a.class.name
f = (raise MyErr rescue $!)
p f.class.name
g = (raise Own rescue $!)
p g.class.name
o = (raise Other rescue $!)
p o.class.name
h = ({}.fetch(:x) rescue $!)
p h.class.name
begin; raise Exception, "q"; rescue Exception => z; end
p z.class.name
p [e, a, f, g, o].map { |x| x.class.name }
k = e.class
p k.name
k = g.class
p k.name
def nm(x) = x.class.name
p nm(e), nm(g), nm(Plain.new), nm(3)
p Plain.new.class.name, 1.class.name, "s".class.name
