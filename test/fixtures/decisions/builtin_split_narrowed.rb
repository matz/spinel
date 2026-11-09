# A boxed send beside a top-level def of a name some builtin class answers splits on
# the builtin classes its receiver may be (the class facts' builtin part): none of
# the exception classes reaches `message` here, so it calls the def at once; an IO
# may reach `fileno`, so that one keeps the split.
def message = :top_message
def fileno = :top_fileno
class A; end
def no_exception(x) = x.send(:message)
def maybe_io(x) = x.send(:fileno)
p [no_exception(A.new), no_exception(3)]
p [maybe_io(A.new), maybe_io($stdout)]
