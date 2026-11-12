# A block a class-body DSL stores and a reader answers back, instance_eval'd on an
# instance (OmniAuth's uid/info): it runs on that instance, past an unrelated
# method of the same name that takes a block and does not store it
class Log
  def info(msg = nil, &block)
    puts(block ? block.call : msg)
  end
end
module Strategy
  def self.included(base)
    base.extend(ClassMethods)
  end
  module ClassMethods
    def uid(&block)
      return @uid_proc unless block
      @uid_proc = block
    end
    def info(&block)
      return @info_proc unless block
      @info_proc = block
    end
    def compile_stack(ancestors, method, context)
      ancestors.inject([]) do |a, ancestor|
        if ancestor.respond_to?(method) && (case method when :uid then ancestor.uid when :info then ancestor.info else raise "dynamic" end)
          a << context.instance_eval(&case method when :uid then ancestor.uid when :info then ancestor.info else raise "dynamic" end)
        end
        a
      end
    end
  end
  def uid = self.class.compile_stack(self.class.ancestors, :uid, self).last
  def info = self.class.compile_stack(self.class.ancestors, :info, self).last
end
class Dev
  include Strategy
  def params = { "email" => "dev@example.com", "name" => "Dev" }
  uid do
    params["email"]
  end
  info do
    { "name" => params["name"] }
  end
end
d = Dev.new
p d.uid
p d.info
Log.new.info { "logged" }
