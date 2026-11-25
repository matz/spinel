# spinel: share
# spinel: gc-stress

class Ghost
  def method_missing(name, *args, &block)
    return "mm:boo" if name == :boo
    return [args, block.call] if name == :mm_apply
    super
  end
end

class Real
  def boo = "real"
  def mm_apply(*args, &block) = [args, block.call]
  def only_real = :real
end

def pick(i) = i.even? ? Ghost.new : Real.new

p pick(0).boo
p pick(1).boo

def traced_pick(i)
  p [:receiver, i]
  return nil if i == 2
  pick(i)
end

def arg(i)
  p [:argument, i]
  i
end

def invoke(i, &block)
  traced_pick(i)&.mm_apply(arg(1), *[arg(2)], k: arg(3), &block)
end

p invoke(0) { :ghost_block }
p invoke(1) { :real_block }
p invoke(2) { :nil_block }

class NilClass
  def method_missing(name, *args)
    :nil_hook
  end
end

p traced_pick(2)&.boo
p [Ghost.new, Real.new][0].boo
p (arg(4).even? ? Ghost.new : Real.new).boo
p pick(pick(0).boo.size).boo
p pick(1).boo.size

class Wrapper
  def initialize(i) = @i = i
  def inner = pick(@i)
end

p Wrapper.new(0).inner.boo

p [0, 1].map { |i| pick(i).boo }
run = ->(i) { pick(i).boo }
p run.call(0)

class DuringLoad
  p pick(1).boo
end

begin
  pick(0).only_real
rescue NoMethodError => error
  p error.name
end
