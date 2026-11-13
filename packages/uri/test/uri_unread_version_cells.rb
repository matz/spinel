# A Struct beside net/http asks URI's component readers for MailTo before
# the record knows where they run, which makes entries over Generic's and
# MailTo's versions; every call then binds MailTo's own method, so no code
# reads the entries' cells, and the C writes none of them.
require "net/http"
R = Struct.new(:a)
p R.new(1)
p URI::HTTP.build(["u:p", "example.com", 8080, "/a", "q=1", "f"]).to_s
# A global of the program's own named like a cell keeps its writes and their
# side effects, whether no cell has the name or one of the net/http entries'
# cells has it (193 in the default load model, 178 in the graph one)
class C; end
p defined?(C.late)
def C.late = 3
$__spinel_ver_1 = (puts "side 1"; 7)
$__spinel_ver_9 = (puts "side 9"; 1)
$__spinel_ver_193 = (puts "side 193"; 5)
$__spinel_ver_178 = (puts "side 178"; 6)
p C.late
