# A method only a subclass defines, called on the variable of a rescue of
# its base class: the call dispatches on the class the exception was raised
# as, a reader with the subclass's own ivar included.

class Base < StandardError
end

class Sub < Base
  def extra = "sub"
end

class Other < Base
  def extra = "other"
end

begin
  raise Sub
rescue Base => e
  puts e.class
  puts e.extra
end

begin
  raise Other
rescue Base => e
  puts e.extra
end

module Errors
  class ServiceError < StandardError
  end

  class SyncError < ServiceError
    def initialize(message, sync_name)
      super(message)
      @sync_name = sync_name
    end

    attr_reader :sync_name
  end

  class PlainError < ServiceError
  end
end

def call(kind)
  raise Errors::SyncError.new("boom", "abc") if kind == 1
  raise Errors::PlainError, "plain"
end

def caught(kind)
  call(kind)
rescue Errors::ServiceError => e
  e
end

begin
  call(1)
rescue Errors::ServiceError => e
  puts e.message
  puts e.sync_name
end

puts caught(1).sync_name

begin
  call(2)
rescue Errors::ServiceError => e
  begin
    e.sync_name
  rescue NoMethodError
    puts "NoMethodError for #{e.class}"
  end
end
