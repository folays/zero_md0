#!/usr/bin/perl -w
#
# http://linux.derkeiler.com/Mailing-Lists/Kernel/2006-11/msg00966.html

use strict;
use warnings FATAL => qw(uninitialized);

use Data::Dumper;
use Getopt::Long qw(:config no_auto_abbrev require_order);
use List::Util;

my %long_opts = (dev => "sdl");
GetOptions("dev=s" => \$long_opts{dev},
    ) or die "bad options";

my @blockdev = $long_opts{dev};
#my @blockdev = map { "/dev/sd$_" } ("b" .. "k");
if ($long_opts{dev} =~ m/^md/o)
{
    open FILE, "-|", "mdadm --verbose --brief  --detail /dev/$long_opts{dev}" or die;
    my $content = do { local $/ = undef; <FILE> };
    close FILE;
    my ($devices) = $content =~ m|^\s*devices=(.*)$|om;
    @blockdev = $devices =~ m/\G\/dev\/(sd.)\d*(?:,|$)/go;
}

sub get_file_contents($$)
{
    my ($file, $col, $s_op) = @_;

    my @file = $file =~ m/BLOCKDEV/o ? map { (my $s = $file) =~ s/BLOCKDEV/$_/; $s } @blockdev : $file;
    my $total = 0;
    foreach my $file (@file)
    {
	open FILE, "<", $file or die "$file : $!";
	chomp (my $s = do { local $/ = undef; <FILE> });
	close FILE;
	my @v = ($s =~ m/\G\s*(\S+)/go)[@{$col || [0]}];
	@v = map { &{$s_op}($_) } @v if $s_op;
	$total += List::Util::sum(@v);
    }
#    printf "%s for %d files : %d\n", $file, scalar(@file), $total;
    return $total;
}

# max request of the blockdev
my $nr_request = &get_file_contents("/sys/block/BLOCKDEV/queue/nr_requests");
# probably max request of AIO system-wide
# my $system_nr_request = &get_file_contents("/proc/sys/fs/aio-max-nr");

# max_sectors_kb should be the maximum # of sector for an io
# hope it's not wrong (ant not max_hw_sectors_kb, but it's equal in my test anyway, 128)
# I don't know why it's 127 on md0, and not 128 ?
my $max_sectors_kb = &get_file_contents("/sys/block/$long_opts{dev}/queue/max_sectors_kb");

# max # of commands scheduled to a device, which is thus the maximum of (iorequest_cnt - iodone_cnt)
my $queue_depth = &get_file_contents("/sys/block/BLOCKDEV/../../queue_depth");

printf "max request of the blockdev : %d\n", $nr_request;
printf "maximum # of bytes to io_submit (in kB) : %d\n", $nr_request * $max_sectors_kb;
printf "maximum # of bytes queued to the blockdev (in kB) : %d\n", $queue_depth * $max_sectors_kb;

# mokay.... let me explain, I won't tell it twice.
# If you submit 200 request of 10 MB each...
# - You will see /proc/sys/fs/aio-nr = 200 (not really interesting)
# - It's 2000 MB
# - If you have a max_sectors_kb of 128 kB, you will need to schedule 2000 * 1024 / 128 = 16000 io requests (nr_requests)
# `- You will then see device's iorequest_cnt and iodone_cnt increase of 16087 (why 87 more than expected, I don't know ?)
#  - You thus need to have nr_request set to more than 160000 for io_submit() to be non-blocking
# - The kernel will schedule at most only queue_depth (128) io simultaneously / 16000 to the device (seen in nr_request)
# `- The total of currently io submitted is seen in `infligh', which is the # of 128 kb (max_sectors_kb) buffers.

sub get($)
{
    my ($file) = @_;

    return &get_file_contents("/sys/block/BLOCKDEV/../../io${file}_cnt", undef, sub { hex $_[0] });
}

# iorequest_cnt seems to only increase once a command is really issued to the blockdev (subject to a maximum of queue_depth at once)
# this number is larger than the number of io_submit(), because each io_submit() buffer must be chopped into smaller buffers of max_sectors_kb.
# I think that it doesn't allow me to immediatelly get the numbers of iorequest needed by my io_submit(), because they are not yet subdivided.
my $ireq = &get("request");
my $idone = &get("done");
# I think i lack of a variable somewhere to tell me how much io there is still to do.
# `- I think it's `inflight' indeed.

printf "\e[33mWARNING\e[0m: iorequest_cnt != iodone_cnt\n" unless $ireq == $idone;
$ireq = $idone;

while (1)
{
    my $aio_nr = &get_file_contents("/proc/sys/fs/aio-nr");
    my $req = &get("request") - $ireq;
    my $done = &get("done") - $idone;
    # inflight seems to be the number of $max_sectors_kb in flight... due to io_submit()
    my ($inflight) = &get_file_contents("/sys/block/BLOCKDEV/inflight", [0, 1]);

    my $stripe_cache = "";
    if ($long_opts{dev} =~ m/^md/o)
    {
	my $stripe_cache_active = &get_file_contents("/sys/block/$long_opts{dev}/md/stripe_cache_active");
	my $stripe_cache_size = &get_file_contents("/sys/block/$long_opts{dev}/md/stripe_cache_size");
	$stripe_cache = sprintf " (stripe_cache %.02f%%)", $stripe_cache_active / $stripe_cache_size * 100;
    }

    printf "submitted [%d] done/request/queued [%06d/%06d/%04d] (queued/queue_depth %.02f%%) (inflight %04d) (inflight/nr_request %.02f%%)%s\n",
    $aio_nr,
    $done, $req, $req - $done,
    ($req - $done) / $queue_depth * 100,
    $inflight,
    $inflight / $nr_request * 100,
    $stripe_cache;
    select undef, undef, undef, .100;
}
