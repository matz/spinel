# `self.members` in a record's class method reads the self it runs on: the
# class that made a proc when it is called, the receiver when instance_eval
# or instance_exec rebinds it. A bare `members` outside a block is the
# class's own.

Entry = Data.define(:code) do
  def self.explicit = proc { self.members }
  def self.arrow = -> { self.members.map { |m| m.to_s } }
  def self.checked = proc { self.members.sort == [:code] }
  def self.sized = Proc.new { [self.members.size, self.members.first] }
  def self.guarded = proc { |on| on ? self.members : :skip }
  def self.direct = members
  def self.compared = members == [:code]
end
Other = Data.define(:other, :extra)
Pair = Struct.new(:left, :right) do
  def self.explicit = lambda { self.members }
  def self.direct = members
end
class Wide < Pair
end
class Plain
end

body = Entry.explicit
p body.call
p Other.instance_eval(&body)
p Pair.instance_eval(&body)
p body.call
p Other.instance_exec(&Entry.arrow)
p Entry.arrow.call
p Other.instance_eval(&Entry.checked)
p Entry.checked.call
p Other.instance_eval(&Entry.sized)
p Entry.sized.call
p Other.instance_exec(false, &Entry.guarded)
p Other.instance_exec(true, &Entry.guarded)
p Entry.guarded.call(true)

p Entry.instance_exec(&Pair.explicit)
p Wide.instance_exec(&Pair.explicit)
p Pair.explicit.call

# an instance answers its own members; a class without the method has none
p Other.new(1, 2).instance_eval(&Entry.explicit)
p Pair.new(1, 2).instance_eval(&Entry.explicit)
begin
  Plain.instance_eval(&Entry.explicit)
rescue NoMethodError => e
  puts e.class
end

# a block instance_eval or instance_exec is given directly
class Entry
  def self.on_other = Other.instance_eval { self.members }
  def self.on_instance = Pair.new(1, 2).instance_eval { self.members }
  def self.on_same = Entry.instance_eval { self.members == [:code] }
  def self.inline = Other.instance_exec(&proc { self.members })
  def self.on(x) = x.instance_eval { members }
  def self.on_plain = Plain.instance_eval { self.members }
end
p Entry.on_other
p Entry.on_instance
p Entry.on_same
p Entry.inline
p Entry.on(Other)
p Entry.on(Pair)
p Entry.on(Pair.new(1, 2))
p Entry.on(Entry)
begin
  Entry.on_plain
rescue NoMethodError => e
  puts e.class
end

p Entry.direct
p Entry.compared
p Pair.direct
p Wide.direct

# removed from the class, the name is undefined again
Gone = Data.define(:a)
class << Gone
  undef_method :members
  def read = members
end
begin
  p Gone.read
rescue NameError => e
  puts e.class
end
