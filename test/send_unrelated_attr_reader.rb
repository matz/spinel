# a name only an unrelated class declares as an attr_reader is still a
# NoMethodError for a self-call and for a send, as a reader called with
# an argument is not reached through a subclass's
class Config
  attr_reader :level
end
class Worker
  def run
    begin
      level(3)
    rescue NoMethodError
      puts "no level"
    end
    begin
      send(:level)
    rescue NoMethodError
      puts "no level by send"
    end
  end
end
Worker.new.run
