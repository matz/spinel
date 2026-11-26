# A block that a stored block hands to a method running it with
# instance_eval(&block) runs on that method's object, not on the self the
# stored block's instance_exec gives.
class Names
  def initialize = (@prefix = "col")
  def created_at = :column
  def name_of(x) = "#{@prefix}_#{x}"
end

class Query
  def initialize(label) = (@label = label)
  def label = @label
  def where(&block)
    Names.new.instance_eval(&block)
  end
end

def keep(&block) = block

body = keep { where { created_at } }
p Query.new("q").instance_exec(&body)

body = keep { where { self.created_at } }
p Query.new("q").instance_exec(&body)

body = keep { where { self.class } }
p Query.new("q").instance_exec(&body)

body = keep { [label, where { created_at }] }
p Query.new("both").instance_exec(&body)

body = keep { where { [1, 2].map { |i| name_of(i) } } }
p Query.new("q").instance_exec(&body)

body = keep { where { @prefix } }
p Query.new("q").instance_exec(&body)
