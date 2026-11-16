# Compiled with --defer-refusals: a define_method in a method's body handed
# the Proc that method is given (the defined method's body is known only at
# run time) is deferred like any other refusal -- the statement raises
# NotImplementedError when the method runs -- and a program that reaches the
# method only through a send of a run-time name builds and runs. Without
# the flag it is refused.
# spinel: defer-refusals: 2:start SELECT 1 true after
class Database
  def create_aggregate(name, step = nil)
    proxy = Class.new do
      def initialize; @ctx = 1; end
    end
    proxy.class_eval do
      define_method(:step_with_ctx, step)
    end
    proxy
  end
  def execute(sql) = sql.upcase
end

puts "start"
db = Database.new
puts db.execute("select 1")
name = ["create", "aggregate"].join("_")
begin
  db.send(name, "sum", proc { |a| a })
rescue NotImplementedError => e
  puts e.message.include?("define_method with a block or Proc the method is handed")
end
puts "after"
proc { "a" }.ruby2_keywords
puts "not reached"
