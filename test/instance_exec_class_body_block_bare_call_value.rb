# A bare call in a class-body block that instance_exec runs on an instance
# reaches the instance's method, and its value is that method's, though a
# class method of the same name answers another class.
class Record
  def self.keep(&block) = (@body = block)
  def self.body = @body
  def self.insert(attrs) = attrs
  def self.peek(&block) = (@peek = block)
  def self.peek_body = @peek

  def insert(attrs) = Record.new
  def name_of = :instance_side

  keep do
    insert({ title: "t" }).name_of
  end

  peek do
    r = insert({ title: "t" })
    [r.name_of, r.class]
  end
end

p Record.new.instance_exec(&Record.body)
p Record.new.instance_exec(&Record.peek_body)
