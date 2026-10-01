# A writer called on a receiver of no single type, as the value of a
# method, where one class answers it with `def` and another with
# attr_accessor: the dispatch's value is the argument, and the class
# reached through the fallback arm still has its writer run.

class Plain
  def body=(v)
    @body = v
    $writers += 1
    $order += "w"
    :not_the_rhs
  end

  def body
    @body
  end
end

class Accessor
  attr_accessor :body
end

def target(i)
  i == 0 ? Plain.new : Accessor.new
end

def assign(i, value)
  target(i).body = value
end

def assign_and_read(i, value)
  t = target(i)
  t.body = value
  t.body
end

$writers = 0
$order = ""
p assign(0, "x")
p assign(1, "y")
p assign_and_read(0, "a")
p assign_and_read(1, "b")

# Receiver and RHS effects must run once, in order, even when the builtin
# emission re-enters codegen to reach an accessor writer through default.
$receivers = 0
$values = 0
$writers = 0
$order = ""

def counted_target(i)
  $receivers += 1
  $order += "r"
  target(i)
end

def next_value
  $values += 1
  $order += "v"
  "rhs-#{$values}"
end

def counted_assign(i)
  counted_target(i).body = next_value
end

def counted_statement(i)
  counted_target(i).body = next_value
  :statement_tail
end

p counted_assign(0)
p counted_assign(1)
p counted_statement(0)
p counted_statement(1)
p [$receivers, $values, $writers, $order]

# A real builtin receiver must keep its writer too. The user writer's
# return deliberately differs from its RHS in both type and value.
class DefaultOwner
  attr_reader :default

  def default=(v)
    @default = v
    $writers += 1
    $order += "w"
    :not_the_rhs
  end
end

def default_target(flag)
  flag ? DefaultOwner.new : {}
end

def counted_default(t)
  $receivers += 1
  $order += "r"
  t
end

def default_assign(t)
  counted_default(t).default = next_value
end

def default_statement(t)
  counted_default(t).default = next_value
  :statement_tail
end

$receivers = 0
$values = 0
$writers = 0
$order = ""
user = default_target(true)
builtin = default_target(false)
p default_assign(user)
p user.default
p default_assign(builtin)
p builtin[:missing]
p default_statement(user)
p user.default
p default_statement(builtin)
p builtin[:missing]
p [$receivers, $values, $writers, $order]
