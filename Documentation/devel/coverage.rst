Determining Source Code Coverage
================================

.. note::
	Coverage instrumentation is currently only supported with LLVM
        and sandbox.

To collect coverage information, barebox must be built with ``CONFIG_GCOV=y``.
The linking process will take much longer than usual, but once done, running
barebox will produce coverage information.

.. code-block:: bash

	images/fuzz-filetype -max_total_time=60 -max_len=2048

This will produce a ``default.profraw`` file, which needs to be further
processed:

.. code-block:: bash

	make coverage-html

This will produce a ``${KBUILD_OUTPUT}/coverage_html/`` directory, which can be
inspected by a web browser:

.. code-block:: bash

	firefox coverage_html/index.html
