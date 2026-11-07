# `raise K` with K a constant a read can reach before its assignment
# evaluates K: where K is not assigned yet that read is CRuby's NameError,
# and once K holds a class, the class is raised. K was raised by its name,
# a class called "XE", which nothing rescued.
def early1
  raise XE, "early"
rescue NameError => e
  p [:early1, e.class, e.message]
rescue ArgumentError => e
  p [:early1, :argument_error, e.message]
end
def early2
  raise XE
rescue NameError => e
  p [:early2, e.class, e.message]
end
early1
early2
XE = ArgumentError
early1
begin
  raise XE, "late"
rescue XE => e
  p [e.class, e.message]
end
begin
  early2
rescue ArgumentError => e
  p [:late2, e.class]
end
