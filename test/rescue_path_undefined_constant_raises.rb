# A rescue clause naming a path the program does not define raises NameError
# when an exception reaches it, as CRuby reads the path then, left to right,
# the exception being handled its cause. The clause matched by the path's
# last name instead: `rescue Outer::Missing` caught a top-level Missing or
# one nested in another module, and `rescue Outer::Mailer::TimeoutError`,
# with only Outer::Net::TimeoutError defined, caught the exception and ran
# no clause at all.
module Outer
  module Net
    class TimeoutError < StandardError; end
  end
  Made = Class.new(StandardError)
end
class Missing < StandardError; end
class AppError < StandardError; end
class Holder; end
module Left; class Clash < StandardError; end; end
module Right; class Clash < StandardError; end; end
module Base; class Shared < StandardError; end; end
class Includer; include Base; end
class Parent; class Inner < StandardError; end; end
class Child < Parent; end
module Outer; Renamed = ::AppError; end
class Dynamic < StandardError; end
module Dyn; end
Dyn.const_set(:Dynamic, Dynamic)
class Put < StandardError; end
Made = Module.new
Made::Put = ::Put

def report(label)
  puts "#{label}: #{yield}"
rescue NameError => e
  puts "#{label}: NameError #{e.message}, cause #{e.cause.class}"
rescue StandardError => e
  puts "#{label}: #{e.class} escaped"
end

# the last name is no constant of the module, though a class of that name
# lives at the top level
begin
  begin
    raise Missing, "m"
  rescue Outer::Missing => e
    puts "rescued by Outer::Missing"
  rescue NameError => e
    puts "a later clause of the same begin does not see it"
  end
rescue NameError => e
  puts "top level: #{e.message}, cause #{e.cause.class}"
end

def leaf_at_top_level
  raise Missing, "m"
rescue Outer::Missing => e
  "caught #{e.class}"
end
report("module, class at top level") { leaf_at_top_level }

# nor of a class
def leaf_under_class
  raise Missing, "m"
rescue Holder::Missing
  "caught"
end
report("class, class at top level") { leaf_under_class }

# a name nothing defines: the next clause is not tried
def leaf_nowhere
  raise AppError, "boom"
rescue Outer::Nowhere
  "caught"
rescue AppError
  "next clause"
end
report("name defined nowhere") { leaf_nowhere }

# the last name is a class nested in another module or class: no constant
# of this one either, and the next clause is not tried
def nested_elsewhere(k)
  raise k, "n"
rescue Right::Shared
  "caught"
rescue StandardError
  "next clause"
end
report("class of another module") { nested_elsewhere(Base::Shared) }
report("class of another module, another raised") { nested_elsewhere(AppError) }

def nested_in_a_class
  raise Parent::Inner, "n"
rescue Holder::Inner => e
  "caught #{e.class}"
end
report("class of another class") { nested_in_a_class }

def nested_elsewhere_later_operand
  raise Base::Shared, "n"
rescue AppError, Holder::Shared
  "caught"
end
report("class of another module, a later operand") { nested_elsewhere_later_operand }

# a builtin exception is no constant of a module of the program
def builtin_leaf
  raise ArgumentError, "a"
rescue Outer::ArgumentError
  "caught"
end
report("builtin under a module") { builtin_leaf }

# a middle name is missing and the last names a class nested elsewhere
def middle_missing(log)
  begin
    raise AppError, "boom"
  rescue Outer::Mailer::TimeoutError => e
    log << "Mailer clause"
  rescue AppError => e
    log << "AppError clause"
  end
  nil
end
log = []
begin
  middle_missing(log)
rescue StandardError => e
  log << e.class.name << e.message
end
p log

def middle_missing_deeper
  raise AppError, "boom"
rescue Outer::Mailer::Net::TimeoutError
  "caught"
rescue AppError
  "next clause"
end
report("two names past the missing one") { middle_missing_deeper }

# a middle name that names a module nested elsewhere
def middle_nested_elsewhere
  raise Outer::Net::TimeoutError, "t"
rescue Right::Net::TimeoutError
  "caught"
end
report("middle name of another module") { middle_nested_elsewhere }

def middle_then_nowhere
  raise AppError, "boom"
rescue Outer::Net::Nowhere::Deep
  "caught"
end
report("missing under a nested module") { middle_then_nowhere }

# an operand before it that matches leaves it unread; one that does not
# match reads it
def second_operand(k)
  raise k, "x"
rescue ArgumentError, Outer::Missing
  "caught"
end
report("earlier operand matches") { second_operand(ArgumentError) }
report("earlier operand does not") { second_operand(TypeError) }

# before a catch-all it is read first
def before_exception
  raise "z"
rescue Outer::Missing, Exception
  "caught"
end
report("before Exception") { before_exception }

# an earlier clause that matches, and a body that raises nothing, never
# reach it
def earlier_clause
  raise AppError, "boom"
rescue AppError
  "first clause"
rescue Outer::Missing
  "second clause"
end
report("earlier clause matches") { earlier_clause }

def quiet
  yield
rescue Outer::Missing, Outer::Mailer::TimeoutError
  "caught"
end
report("nothing raised") { quiet { 42 } }

# a begin whose value is used
def as_value
  v = begin
    raise Missing, "m"
  rescue Outer::Missing
    "caught"
  end
  "value #{v}"
end
report("begin as a value") { as_value }

# paths the program defines still match by the class they name
def clash(k)
  raise k, "c"
rescue Left::Clash
  "Left"
rescue Right::Clash
  "Right"
end
report("Left::Clash") { clash(Left::Clash) }
report("Right::Clash") { clash(Right::Clash) }

def defined_paths(k)
  raise k, "d"
rescue Outer::Net::TimeoutError
  "Outer::Net::TimeoutError"
rescue Outer::Made
  "Outer::Made"
rescue Includer::Shared
  "Includer::Shared"
rescue Child::Inner
  "Child::Inner"
rescue Outer::Renamed
  "Outer::Renamed"
rescue ::Missing
  "::Missing"
rescue Dyn::Dynamic
  "Dyn::Dynamic"
rescue Made::Put
  "Made::Put"
end
report("nested") { defined_paths(Outer::Net::TimeoutError) }
report("Class.new constant") { defined_paths(Outer::Made) }
report("through an included module") { defined_paths(Base::Shared) }
report("through a superclass") { defined_paths(Parent::Inner) }
report("alias in a module") { defined_paths(AppError) }
report("rooted") { defined_paths(Missing) }
report("const_set") { defined_paths(Dynamic) }
report("written into a Module.new") { defined_paths(Put) }

# a class of the runtime under a builtin module
def domain
  Math.sqrt(-1.0)
rescue Math::DomainError => e
  "caught #{e.class}"
end
report("Math::DomainError") { domain }
