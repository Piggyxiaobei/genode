/*
 * \brief  Child creation framework
 * \author Norman Feske
 * \date   2006-07-22
 */

/*
 * Copyright (C) 2006-2017 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _INCLUDE__BASE__CHILD_H_
#define _INCLUDE__BASE__CHILD_H_

#include <base/rpc_server.h>
#include <base/heap.h>
#include <base/service.h>
#include <base/lock.h>
#include <base/local_connection.h>
#include <util/arg_string.h>
#include <ram_session/connection.h>
#include <region_map/client.h>
#include <pd_session/connection.h>
#include <cpu_session/connection.h>
#include <log_session/connection.h>
#include <rom_session/connection.h>
#include <parent/capability.h>

namespace Genode {
	struct Child_policy;
	struct Child;
}


/**
 * Child policy interface
 *
 * A child-policy object is an argument to a 'Child'. It is responsible for
 * taking policy decisions regarding the parent interface. Most importantly,
 * it defines how session requests are resolved and how session arguments
 * are passed to servers when creating sessions.
 */
struct Genode::Child_policy
{
	typedef String<64> Name;
	typedef String<64> Binary_name;
	typedef String<64> Linker_name;

	virtual ~Child_policy() { }

	/**
	 * Name of the child used as the child's label prefix
	 */
	virtual Name name() const = 0;

	/**
	 * ROM module name of the binary to start
	 */
	virtual Binary_name binary_name() const { return name(); }

	/**
	 * ROM module name of the dynamic linker
	 */
	virtual Linker_name linker_name() const { return "ld.lib.so"; }

	/**
	 * Determine service to provide a session request
	 *
	 * \return  service to be contacted for the new session
	 * \deprecated
	 *
	 * \throw Parent::Service_denied
	 */
	virtual Service &resolve_session_request(Service::Name       const &,
	                                         Session_state::Args const &)
	{
		throw Parent::Service_denied();
	}

	/**
	 * Routing destination of a session request
	 */
	struct Route
	{
		Service &service;
		Session_label const label;
	};

	/**
	 * Determine service and server-side label for a given session request
	 *
	 * \return  routing and policy-selection information for the session
	 *
	 * \throw Parent::Service_denied
	 */
	virtual Route resolve_session_request(Service::Name const &,
	                                      Session_label const &)
	{
		/* \deprecated  make pure virtual once the old version is gone */
		throw Parent::Service_denied();
	}

	/**
	 * Apply transformations to session arguments
	 */
	virtual void filter_session_args(Service::Name const &,
	                                 char * /*args*/, size_t /*args_len*/) { }

	/**
	 * Register a service provided by the child
	 */
	virtual void announce_service(Service::Name const &) { }

	/**
	 * Apply session affinity policy
	 *
	 * \param affinity  affinity passed along with a session request
	 * \return          affinity subordinated to the child policy
	 */
	virtual Affinity filter_session_affinity(Affinity const &affinity)
	{
		return affinity;
	}

	/**
	 * Exit child
	 */
	virtual void exit(int exit_value)
	{
		log("child \"", name(), "\" exited with exit value ", exit_value);
	}

	/**
	 * Reference RAM session
	 *
	 * The RAM session returned by this method is used for session-quota
	 * transfers.
	 */
	virtual Ram_session           &ref_ram() = 0;
	virtual Ram_session_capability ref_ram_cap() const = 0;

	/**
	 * Respond to the release of resources by the child
	 *
	 * This method is called when the child confirms the release of
	 * resources in response to a yield request.
	 */
	virtual void yield_response() { }

	/**
	 * Take action on additional resource needs by the child
	 */
	virtual void resource_request(Parent::Resource_args const &) { }

	/**
	 * Initialize the child's RAM session
	 *
	 * The function must define the child's reference account and transfer
	 * the child's initial RAM quota.
	 */
	virtual void init(Ram_session &, Capability<Ram_session>) = 0;

	/**
	 * Initialize the child's CPU session
	 *
	 * The function may install an exception signal handler or assign CPU quota
	 * to the child.
	 */
	virtual void init(Cpu_session &, Capability<Cpu_session>) { }

	/**
	 * Initialize the child's PD session
	 *
	 * The function may install a region-map fault handler for the child's
	 * address space ('Pd_session::address_space');.
	 */
	virtual void init(Pd_session &, Capability<Pd_session>) { }

	class Nonexistent_id_space : Exception { };

	/**
	 * ID space for sessions provided by the child
	 *
	 * \throw Nonexistent_id_space
	 */
	virtual Id_space<Parent::Server> &server_id_space() { throw Nonexistent_id_space(); }

	/**
	 * Notification hook invoked each time a session state is modified
	 */
	virtual void session_state_changed() { }

	/**
	 * Granularity of allocating the backing store for session meta data
	 *
	 * Session meta data is allocated from 'ref_ram'. The first batch of
	 * session-state objects is allocated at child-construction time.
	 */
	virtual size_t session_alloc_batch_size() const { return 16; }

	/**
	 * Return true to create the environment sessions at child construction
	 *
	 * By returning 'false', it is possible to create 'Child' objects without
	 * routing of their environment sessions at construction time. Once the
	 * routing information is available, the child's environment sessions
	 * must be manually initiated by calling 'Child::initiate_env_sessions()'.
	 */
	virtual bool initiate_env_sessions() const { return true; }

	/**
	 * Return region map for the child's address space
	 *
	 * \param pd  the child's PD session capability
	 *
	 * By default, the function returns a 'nullptr'. In this case, the 'Child'
	 * interacts with the address space of the child's PD session via RPC calls
	 * to the 'Pd_session::address_space'.
	 *
	 * By overriding the default, those RPC calls can be omitted, which is
	 * useful if the child's PD session (including the PD's address space) is
	 * virtualized by the parent. If the virtual PD session is served by the
	 * same entrypoint as the child's parent interface, an RPC call to 'pd'
	 * would otherwise produce a deadlock.
	 */
	virtual Region_map *address_space(Pd_session &) { return nullptr; }
};


/**
 * Implementation of the parent interface that supports resource trading
 *
 * There are three possible cases of how a session can be provided to
 * a child: The service is implemented locally, the session was obtained by
 * asking our parent, or the session is provided by one of our children.
 *
 * These types must be differentiated for the quota management when a child
 * issues the closing of a session or transfers quota via our parent
 * interface.
 *
 * If we close a session to a local service, we transfer the session quota
 * from our own account to the client.
 *
 * If we close a parent session, we receive the session quota on our own
 * account and must transfer this amount to the session-closing child.
 *
 * If we close a session provided by a server child, we close the session
 * at the server, transfer the session quota from the server's RAM session
 * to our account, and subsequently transfer the same amount from our
 * account to the client.
 */
class Genode::Child : protected Rpc_object<Parent>,
                      Session_state::Ready_callback,
                      Session_state::Closed_callback
{
	private:

		struct Initial_thread_base
		{
			/**
			 * Start execution at specified instruction pointer
			 */
			virtual void start(addr_t ip) = 0;

			/**
			 * Return capability of the initial thread
			 */
			virtual Capability<Cpu_thread> cap() const = 0;
		};

		struct Initial_thread : Initial_thread_base
		{
			private:

				Cpu_session      &_cpu;
				Thread_capability _cap;

			public:

				typedef Cpu_session::Name Name;

				/**
				 * Constructor
				 *
				 * \throw Cpu_session::Thread_creation_failed
				 * \throw Cpu_session::Out_of_metadata
				 */
				Initial_thread(Cpu_session &, Pd_session_capability, Name const &);
				~Initial_thread();

				void start(addr_t) override;

				Capability<Cpu_thread> cap() const { return _cap; }
		};

		/* child policy */
		Child_policy &_policy;

		/* print error message with the child's name prepended */
		template <typename... ARGS>
		void _error(ARGS &&... args) { error(_policy.name(), ": ", args...); }

		Region_map &_local_rm;

		Rpc_entrypoint    &_entrypoint;
		Parent_capability  _parent_cap;

		/* signal handlers registered by the child */
		Signal_context_capability _resource_avail_sigh;
		Signal_context_capability _yield_sigh;
		Signal_context_capability _session_sigh;

		/* arguments fetched by the child in response to a yield signal */
		Lock          _yield_request_lock;
		Resource_args _yield_request_args;

		/* sessions opened by the child */
		Id_space<Client> _id_space;

		/* allocator used for dynamically created session state objects */
		Sliced_heap _session_md_alloc { _policy.ref_ram(), _local_rm };

		Session_state::Factory::Batch_size const
			_session_batch_size { _policy.session_alloc_batch_size() };

		/* factory for dynamically created  session-state objects */
		Session_state::Factory _session_factory { _session_md_alloc,
		                                          _session_batch_size };

		typedef Session_state::Args Args;

		static Child_policy::Route _resolve_session_request(Child_policy &,
                                                            Service::Name const &,
                                                            char const *);
		/*
		 * Members that are initialized not before the child's environment is
		 * complete.
		 */

		void _try_construct_env_dependent_members();

		Constructible<Initial_thread> _initial_thread;

		struct Process
		{
			class Missing_dynamic_linker : Exception { };
			class Invalid_executable     : Exception { };

			struct Loaded_executable
			{
				/**
				 * Initial instruction pointer of the new process, as defined
				 * in the header of the executable.
				 */
				addr_t entry;

				/**
				 * Constructor parses the executable and sets up segment
				 * dataspaces
				 *
				 * \param local_rm  local address space, needed to make the
				 *                  segment dataspaces temporarily visible in
				 *                  the local address space to initialize their
				 *                  content with the data from the 'elf_ds'
				 *
				 * \throw Region_map::Attach_failed
				 * \throw Invalid_executable
				 * \throw Missing_dynamic_linker
				 * \throw Ram_session::Alloc_failed
				 */
				Loaded_executable(Dataspace_capability elf_ds,
				                  Dataspace_capability ldso_ds,
				                  Ram_session &ram,
				                  Region_map &local_rm,
				                  Region_map &remote_rm,
				                  Parent_capability parent_cap);
			} loaded_executable;

			/**
			 * Constructor
			 *
			 * \param ram     RAM session used to allocate the BSS and
			 *                DATA segments for the new process
			 * \param parent  parent of the new protection domain
			 * \param name    name of protection domain
			 *
			 * \throw Missing_dynamic_linker
			 * \throw Invalid_executable
			 * \throw Region_map::Attach_failed
			 * \throw Ram_session::Alloc_failed
			 *
			 * The other arguments correspond to those of 'Child::Child'.
			 *
			 * On construction of a protection domain, the initial thread is
			 * started immediately.
			 *
			 * The argument 'elf_ds' may be invalid to create an empty process.
			 * In this case, all process initialization steps except for the
			 * creation of the initial thread must be done manually, i.e., as
			 * done for implementing fork.
			 */
			Process(Dataspace_capability  elf_ds,
			        Dataspace_capability  ldso_ds,
			        Pd_session_capability pd_cap,
			        Pd_session           &pd,
			        Ram_session          &ram,
			        Initial_thread_base  &initial_thread,
			        Region_map           &local_rm,
			        Region_map           &remote_rm,
			        Parent_capability     parent);

			~Process();
		};

		Constructible<Process> _process;

		/*
		 * The child's environment sessions
		 */

		template <typename CONNECTION>
		struct Env_connection
		{
			Child &_child;

			Id_space<Parent::Client>::Id const _client_id;

			typedef String<64> Label;

			Args const _args;

			/*
			 * The 'Env_service' monitors session responses in order to attempt
			 * to 'Child::_try_construct_env_dependent_members()' on the
			 * arrival of environment sessions.
			 */
			struct Env_service : Service, Session_state::Ready_callback
			{
				Child   &_child;
				Service &_service;

				Env_service(Child &child, Service &service)
				:
					Genode::Service(CONNECTION::service_name(), service.ram()),
					_child(child), _service(service)
				{ }

				void initiate_request(Session_state &session) override
				{
					session.ready_callback = this;
					session.async_client_notify = true;
					_service.initiate_request(session);

					if (session.phase == Session_state::INVALID_ARGS)
						error(_child._policy.name(), ": environment ",
						      CONNECTION::service_name(), " session denied "
						      "(", session.args(), ")");
				}

				/**
				 * Session_state::Ready_callback
				 */
				void session_ready(Session_state &session) override
				{
					_child._try_construct_env_dependent_members();
				}

				void wakeup() override { _service.wakeup(); }

				bool operator == (Service const &other) const override
				{
					return _service == other;
				}
			};

			Constructible<Env_service> _env_service;

			Constructible<Local_connection<CONNECTION> > _connection;

			/**
			 * Construct session arguments with the child policy applied
			 */
			Args _construct_args(Child_policy &policy, Label const &label)
			{
				/* copy original arguments into modifiable buffer */
				char buf[Session_state::Args::capacity()];
				buf[0] = 0;

				/* supply label as session argument */
				if (label.valid())
					Arg_string::set_arg_string(buf, sizeof(buf), "label", label.string());

				/* apply policy to argument buffer */
				policy.filter_session_args(CONNECTION::service_name(), buf, sizeof(buf));

				return Session_state::Args(Cstring(buf));
			}

			static char const *_service_name() { return CONNECTION::service_name(); }

			Env_connection(Child &child, Id_space<Parent::Client>::Id id,
			               Label const &label = Label())
			:
				_child(child), _client_id(id),
				_args(_construct_args(child._policy, label))
			{ }

			/**
			 * Initiate routing and creation of the environment session
			 */
			void initiate()
			{
				/* don't attempt to initiate env session twice */
				if (_connection.constructed())
					return;

				Child_policy::Route const route =
					_child._resolve_session_request(_child._policy,
					                                _service_name(),
					                                _args.string());

				_env_service.construct(_child, route.service);
				_connection.construct(*_env_service, _child._id_space, _client_id,
				                      _args, _child._policy.filter_session_affinity(Affinity()));
			}

			typedef typename CONNECTION::Session_type SESSION;

			SESSION &session() { return _connection->session(); }

			Capability<SESSION> cap() const { return _connection->cap(); }
		};

		Env_connection<Ram_connection> _ram    { *this, Env::ram(),    _policy.name() };
		Env_connection<Pd_connection>  _pd     { *this, Env::pd(),     _policy.name() };
		Env_connection<Cpu_connection> _cpu    { *this, Env::cpu(),    _policy.name() };
		Env_connection<Log_connection> _log    { *this, Env::log(),    _policy.name() };
		Env_connection<Rom_connection> _binary { *this, Env::binary(), _policy.binary_name() };

		Constructible<Env_connection<Rom_connection> > _linker;

		Dataspace_capability _linker_dataspace()
		{
			return _linker.constructed() ? _linker->session().dataspace()
			                             : Rom_dataspace_capability();
		}

		void _revert_quota_and_destroy(Session_state &);

		void _discard_env_session(Id_space<Parent::Client>::Id);

		Close_result _close(Session_state &);

		/**
		 * Session_state::Ready_callback
		 */
		void session_ready(Session_state &session) override;

		/**
		 * Session_state::Closed_callback
		 */
		void session_closed(Session_state &) override;

	public:

		/**
		 * Constructor
		 *
		 * \param rm          local address space, usually 'env.rm()'
		 * \param entrypoint  entrypoint used to serve the parent interface of
		 *                    the child
		 * \param policy      policy for the child
		 *
		 * \throw Parent::Service_denied  if the initial sessions for the
		 *                                child's environment could not be
		 *                                opened
		 */
		Child(Region_map &rm, Rpc_entrypoint &entrypoint, Child_policy &policy);

		/**
		 * Destructor
		 *
		 * On destruction of a child, we close all sessions of the child to
		 * other services.
		 */
		virtual ~Child();

		/**
		 * Return true if the child has been started
		 *
		 * After the child's construction, the child is not always able to run
		 * immediately. In particular, a session of the child's environment
		 * may still be pending. This method returns true only if the child's
		 * environment is completely initialized at the time of calling.
		 *
		 * If all environment sessions are immediately available (as is the
		 * case for local services or parent services), the return value is
		 * expected to be true. If this is not the case, one of child's
		 * environment sessions could not be established, e.g., the ROM session
		 * of the binary could not be obtained.
		 */
		bool active() const { return _process.constructed(); }

		/**
		 * Initialize the child's RAM session
		 */
		void initiate_env_ram_session();

		/**
		 * Trigger the routing and creation of the child's environment session
		 *
		 * See the description of 'Child_policy::initiate_env_sessions'.
		 */
		void initiate_env_sessions();

		/**
		 * RAM quota unconditionally consumed by the child's environment
		 */
		static size_t env_ram_quota()
		{
			return Cpu_connection::RAM_QUOTA + Ram_connection::RAM_QUOTA +
			        Pd_connection::RAM_QUOTA + Log_connection::RAM_QUOTA +
			     2*Rom_connection::RAM_QUOTA;
		}

		template <typename FN>
		void for_each_session(FN const &fn) const
		{
			_id_space.for_each<Session_state const>(fn);
		}

		/**
		 * Deduce session costs from usable ram quota
		 */
		static size_t effective_ram_quota(size_t const ram_quota)
		{
			if (ram_quota < env_ram_quota())
				return 0;

			return ram_quota - env_ram_quota();
		}

		Ram_session_capability ram_session_cap() const { return _ram.cap(); }

		Parent_capability parent_cap() const { return cap(); }

		Ram_session &ram() { return _ram.session(); }
		Cpu_session &cpu() { return _cpu.session(); }
		Pd_session  &pd()  { return _pd .session(); }

		/**
		 * Request factory for creating session-state objects
		 */
		Session_state::Factory &session_factory() { return _session_factory; }

		/**
		 * Instruct the child to yield resources
		 *
		 * By calling this method, the child will be notified about the
		 * need to release the specified amount of resources. For more
		 * details about the protocol between a child and its parent,
		 * refer to the description given in 'parent/parent.h'.
		 */
		void yield(Resource_args const &args);

		/**
		 * Notify the child about newly available resources
		 */
		void notify_resource_avail() const;


		/**********************
		 ** Parent interface **
		 **********************/

		void announce(Service_name const &) override;
		void session_sigh(Signal_context_capability) override;
		Session_capability session(Client::Id, Service_name const &,
		                           Session_args const &, Affinity const &) override;
		Session_capability session_cap(Client::Id) override;
		Upgrade_result upgrade(Client::Id, Upgrade_args const &) override;
		Close_result close(Client::Id) override;
		void exit(int) override;
		void session_response(Server::Id, Session_response) override;
		void deliver_session_cap(Server::Id, Session_capability) override;
		Thread_capability main_thread_cap() const override;
		void resource_avail_sigh(Signal_context_capability) override;
		void resource_request(Resource_args const &) override;
		void yield_sigh(Signal_context_capability) override;
		Resource_args yield_request() override;
		void yield_response() override;
};

#endif /* _INCLUDE__BASE__CHILD_H_ */
