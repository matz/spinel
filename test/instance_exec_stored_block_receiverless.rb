# A receiverless instance_exec or instance_eval of a stored block in an
# instance method runs the block on self, as self.instance_exec does.
class Record
  def self.bodies
    @bodies ||= {}
  end

  def self.command(name, &body)
    bodies[name] = body
  end

  attr_reader :attrs

  def initialize
    @attrs = {}
  end

  def update(changes)
    @attrs = @attrs.merge(changes)
  end

  def call(name, **args)
    instance_exec(**args, &self.class.bodies[name])
  end
end

class Comment < Record
  command :hide do
    update(hidden: true)
  end

  command :edit do |body:|
    update(body: body)
  end
end

comment = Comment.new
comment.call(:hide)
p comment.attrs
comment.call(:edit, body: "second")
p comment.attrs
class Box2
  BLOCKS = []
  def self.on(&b) = BLOCKS << b
  def initialize = @n = 0
  def bump(k = 1) = @n += k
  def run_all
    BLOCKS.each_index { |i| instance_eval(&BLOCKS[i]) }
    @n
  end
  def run_with(k) = instance_exec(k, &BLOCKS[0])
  attr_reader :n
end
Box2.on { bump }
Box2.on { bump(10) }
b = Box2.new
p b.run_all
b.run_with(5)
p b.n
