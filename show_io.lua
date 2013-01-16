folays = require("folays")

if (not arg[1]) then
   print("Usage: "..arg[0].." <block device>")
   os.exit()
end

local bdev = arg[1]
local all_bdev = {bdev}

function it_list(it)
  local t = {}
  for item in it do
--    print("it_list ["..(#t+1).."] assign "..item)
    t[#t+1] = item
  end
  return t
end

if (string.match(bdev, "md.+")) then
  local f = io.popen("mdadm --verbose --brief --detail /dev/"..bdev)
  if (not f) then error("could not popen mdadm to /dev/"..bdev) end
  local s = f:read("*a")
  f:close()
  local devices = string.match(s, "%sdevices=(.*)$")
  all_bdev = it_list(string.gmatch(devices, "/dev/(sd%a)%d?"))
end

function get_file_contents(filename)
  local f = io.open(filename, "r")
  if (not f) then error("could not open file "..filename) end
  local s = f:read("*a")
  f:close()
--  print("read "..filename.." -> "..s)
  return s
end

function get_file_sums(filename, columns)
  local total = 0
  local k, v
  for k, v in pairs(all_bdev) do
    local s = get_file_contents(string.gsub(filename, "BLOCKDEV", v))
    local cols = it_list(string.gmatch(s, "(%w+)"))
    local k_col, v_col
    for k_col, v_col in pairs(columns or {1}) do
      local n = cols[v_col]
      total = total + n
    end
  end
--  print("total of "..filename.." is "..total)
  return total
end

local nr_requests = get_file_sums("/sys/block/BLOCKDEV/queue/nr_requests")
local max_sectors_kb = get_file_sums("/sys/block/"..bdev.."/queue/max_sectors_kb")
local queue_depth = get_file_sums("/sys/block/BLOCKDEV/../../queue_depth")

print(string.format("Using device /dev/%s", bdev))
print(string.format("max request of the blockdev %d", nr_requests))
print(string.format("maximum # of bytes to io_submit (in kB) : %d", nr_requests * max_sectors_kb))
print(string.format("maximum # of bytes queued to the blockdev (in kB) : %d", queue_depth * max_sectors_kb))

local ireq = get_file_sums("/sys/block/BLOCKDEV/../../iorequest_cnt")
local idone = get_file_sums("/sys/block/BLOCKDEV/../../iodone_cnt")

print(ireq, idone)
if (ireq ~= idone) then print("\027[33mWARNING\027[0m: iorequest_cnt != iodone_cnt") end
ireq = idone

while (true) do
  local aio_nr = get_file_contents("/proc/sys/fs/aio-nr")
  local req = get_file_sums("/sys/block/BLOCKDEV/../../iorequest_cnt") - ireq
  local done = get_file_sums("/sys/block/BLOCKDEV/../../iodone_cnt") - idone
  local inflight = get_file_sums("/sys/block/BLOCKDEV/inflight", {1, 2})

  local stripe_cache = ""
  if (string.match(bdev, "md.+")) then
    local stripe_cache_active = get_file_sums("/sys/block/"..bdev.."/md/stripe_cache_active")
    local stripe_cache_size = get_file_sums("/sys/block/"..bdev.."/md/stripe_cache_size")
    stripe_cache = string.format(" (stripe_cache %.02f%%)", stripe_cache_active / stripe_cache_size * 100)
  end

  print(string.format("submitted [%d] done/request/queued [%06d/%06d/%04d] (queued/queue_depth %.02f%%) (inflight %04d) (inflight/nr_request %.02f%%)%s",
	aio_nr,
	done, req, req - done,
	(req - done) / queue_depth * 100,
	inflight,
	inflight / nr_requests * 100,
	stripe_cache))
  folays.usleep(100000)
end
