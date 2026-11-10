# A class value held as self in a method Object includes, or in a boxed
# slot, answers respond_to? with a name known only at run time, and a
# public_send/send of it reaches the method: one a top-level `class Class`
# reopening gives every class, or the class's own `def self.` method.
class Class
  def descendants
    raise(NotImplementedError, "pruned")
  end

  def tag_name
    "tag:#{name}"
  end
end

module Tryable
  def try(*args, **kwargs, &block)
    if respond_to?(args.first)
      public_send(*args, **kwargs, &block)
    end
  end

  def via_send(*args)
    send(*args)
  end

  def snd(*args)
    public_send(*args)
  end

  def bare_tag
    tag_name
  end
end

class Object
  include Tryable
end

module Plain; end
class A
  def self.table_name
    "as"
  end
end
class B
  def descendants
    [1]
  end
end
class C
  def self.descendants
    [:own]
  end
end

[A, B.new, 1, C, Plain].each do |x|
  begin
    p x.try(:descendants)
  rescue NotImplementedError => e
    p e.message
  end
end

# (called directly too, so compiled: a class method reached only through a
# run-time name is not)
p A.table_name
p A.try(:table_name)
p C.try(:table_name)
p A.snd(:table_name)
p C.snd(:descendants)
p A.try(:tag_name)
p A.via_send(:tag_name)
p A.bare_tag
p Plain.try(:tag_name)

names = [:descendants, :tag_name, :table_name, :new, :nope]
[A, Plain, C, 1].each do |x|
  p names.map { |n| x.respond_to?(n) }
end
