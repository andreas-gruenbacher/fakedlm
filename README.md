# FakeDLM

FakeDLM is a simple replacement for the Distributed Lock Manager (DLM)'s
dlm\_controld that simulates a cluster for testing purposes only.  It assumes
perfect network connectivity and does not rely on a "real" cluster resource and
membership manager, and is not suitable for production use.

FakeDLM is currently implemented as an "interactive" process (not a deamon).
When started on a set of cluster nodes, the FakeDLM instances connect to each
other and start managing the lockgroup membership similar to dlm\_controld.

When a node loses connectivity to any of the other nodes (for example, when the
other node is shut down with ^C), it shuts down all lockspaces and waits for
full connectivity to be reestablished.

By default, the fakedlm instances talk to each other over TCP port 21066.  (The
kernel DLM uses TCP port 21064.)  When a firewall is used, a command similar to
the following may be required to allow the cluster nodes to talk to each other:

```
firewall-cmd --permanent --add-port 21066/tcp
```

## KNOWN PROBLEMS

* No support for DLM deadlock detection so far.

* The kernel DLM does not like to be shut down, at least not by its controlling
  process, and sometimes deadlocks doing that.  This still needs to be
  investigated.
