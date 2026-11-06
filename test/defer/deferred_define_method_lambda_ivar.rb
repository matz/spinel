# Compiled with --defer-refusals: a class body's `%w(...).each do |m|
# ... end` keeps a lambda for a define_method whose name is the loop's
# element, which is refused (a non-literal name, a Proc as the body) and
# raises NotImplementedError where it stands. The lambda is a proc of the
# class body, so its `@datetime` is the body class's own instance variable
# (nil, as on the class object), the way a lambda written straight in the
# body reads it. Taken for the top level's, the read named a slot no code
# declared (civ_Toplevel_datetime, or `self` where none is), and the C did
# not build. Without the flag it is refused.
# spinel: defer-refusals: 2:start sec no-sec true after
puts "start"
begin
  class DateTimeSelector
    def initialize(datetime)
      @datetime = datetime
    end

    %w( sec min hour ).each do |method|
      name = method.to_sym
      reader = lambda do
        case @datetime
        when Hash then @datetime[name]
        when nil then "no-#{name}"
        else @datetime.send(name)
        end
      end
      puts method
      puts reader.call
      define_method(method, reader)
    end
  end
rescue NotImplementedError => e
  puts e.message.include?("define_method")
end
puts "after"
proc { "a" }.ruby2_keywords
puts "not reached"
