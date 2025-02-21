/* jshint expr: true */
/* eslint no-unused-expressions: 0 */
/* global describe, it, beforeEach, afterEach */
'use strict';
const internal = require('internal');
const tasks = require('@arangodb/tasks');
const expect = require('chai').expect;

const isServer = typeof arango === 'undefined';
const query = 'FOR x IN 1..5 LET y = SLEEP(@value) RETURN x';
const taskInfo = {
  offset: 0,
  command: function () {
    var query = 'FOR x IN 1..5 LET y = SLEEP(@value) RETURN x';
    require('internal').db._query(query, { value: 1 }, {profile: true});
  }
};

function filterQueries (q) {
  return (q.query === query);
}

function sendQuery (count, async) {
  count = count || 1;
  for (let i = 0; i < count; ++i) {
    if (async === false) {
      internal.db._query(query, { value: 1 });
    } else {
      tasks.register(taskInfo);
    }
  }
  if (async === true) {
    internal.wait(1, false);
  }
}

function restoreDefaults (testee) {
  testee.properties({
    enabled: true,
    trackSlowQueries: true,
    trackBindVars: true,
    maxSlowQueries: 64,
    slowQueryThreshold: 10,
    maxQueryStringLength: 8192
  });
}

describe('AQL query analyzer', function () {
  let testee;

  beforeEach(function () {
    testee = require('@arangodb/aql/queries');
    restoreDefaults(testee);
  });

  afterEach(function () {
    restoreDefaults(testee);
  });

  it('should be able to activate the tracking', function () {
    testee.properties({
      enabled: true
    });
    expect(testee.properties().enabled).to.be.ok;
  });

  it('should be able to deactivate the tracking', function () {
    testee.properties({
      enabled: false
    });
    expect(testee.properties().enabled).not.to.be.ok;
  });

  it('should be able to set tracking properties', function () {
    testee.properties({
      enabled: true,
      trackSlowQueries: false,
      trackBindVars: false,
      maxSlowQueries: 42,
      slowQueryThreshold: 2.5,
      maxQueryStringLength: 117
    });
    expect(testee.properties().enabled).to.be.ok;
    expect(testee.properties().trackSlowQueries).not.to.be.ok;
    expect(testee.properties().trackBindVars).not.to.be.ok;
    expect(testee.properties().maxSlowQueries).to.equal(42);
    expect(testee.properties().slowQueryThreshold).to.equal(2.5);
    expect(testee.properties().maxQueryStringLength).to.equal(117);
  });

  describe('with active tracking', function () {
    beforeEach(function () {
      if (isServer && internal.debugCanUseFailAt()) {
        internal.debugClearFailAt();
      }
      testee.properties({
        enabled: true,
        slowQueryThreshold: 20
      });
      testee.clearSlow();
    });

    afterEach(function () {
      if (isServer && internal.debugCanUseFailAt()) {
        internal.debugClearFailAt();
      }
      // kill all async tasks that will execute the query that we
      // are looking for
      tasks.get().forEach(function(task) {
        if (task.command.match(/SLEEP\(@value\)/)) {
          try {
            tasks.unregister(task.id);
          } catch (err) {
            // not an error if this fails, as the task may have completed 
            // between `tasks.get()` and `tasks.unregister()`
          }
        }
      });
      // wait a bit for tasks to be finished
      internal.wait(0.2, false);
      // now kill all queries we are looking for that may still be
      // executed
      while (true) {
        const list = testee.current().filter(filterQueries);
        if (list.length === 0) {
          break;
        }
        for (let item of list) {
          try {
            testee.kill(item.id);
          } catch (e) {
            // noop
          }
        }
        internal.wait(0.1, false);
      }
    });

    if (isServer && internal.debugCanUseFailAt()) {
      it('should not crash when inserting a query into the current list fails', function () {
        internal.debugSetFailAt('QueryList::insert');

        // inserting the query will fail
        sendQuery(1, true);
        expect(testee.current().filter(filterQueries).length).to.equal(0);
      });
    }

    it('should be able to get currently running queries', function () {
      sendQuery(1, true);
      let q;
      let counter = 0;
      while (++counter < 10) {
        q = testee.current().filter(filterQueries).length;
        if (q > 0) {
          break;
        }
        internal.wait(0.25, false);
      }
      expect(q).to.equal(1);
    });
    
    it('should have proper running query descriptions', function () {
      let now = (new Date()).toISOString();
      sendQuery(1, true);
      let q;
      let counter = 0;
      while (++counter < 10) {
        q = testee.current().filter(filterQueries);
        if (q.length > 0) {
          break;
        }
        internal.wait(0.25, false);
      }
      expect(q.length).to.equal(1);
      expect(q[0]).to.have.property('id');
      expect(q[0]).to.have.property('database', '_system');
      expect(q[0]).to.have.property('user', 'root');
      expect(q[0]).to.have.property('query', query);
      expect(q[0]).to.have.property('bindVars');
      expect(q[0].bindVars).to.eql({ value: 1 });
      expect(q[0]).to.have.property('started');
      expect(q[0].started).to.be.greaterThan(now);
      expect(q[0]).to.have.property('runTime');
      expect(q[0].runTime).to.be.greaterThan(0.0);
      expect(q[0].runTime).to.be.lessThan(60.0);
      expect(q[0]).to.have.property('peakMemoryUsage');
      expect(q[0].peakMemoryUsage).to.be.lessThan(100 * 1024);
      expect(q[0]).to.have.property('state', 'executing');
      expect(q[0]).to.have.property('stream', false);
    });
    
    it('should have proper running query descriptions, without bind vars', function () {
      let now = (new Date()).toISOString();
      testee.properties({
        trackBindVars: false
      });
      sendQuery(1, true);
      let q;
      let counter = 0;
      while (++counter < 10) {
        q = testee.current().filter(filterQueries);
        if (q.length > 0) {
          break;
        }
        internal.wait(0.25, false);
      }
      expect(q.length).to.equal(1);
      expect(q[0]).to.have.property('id');
      expect(q[0]).to.have.property('database', '_system');
      expect(q[0]).to.have.property('user', 'root');
      expect(q[0]).to.have.property('query', query);
      expect(q[0]).to.have.property('bindVars');
      expect(q[0].bindVars).to.eql({ });
      expect(q[0]).to.have.property('started');
      expect(q[0].started).to.be.greaterThan(now);
      expect(q[0]).to.have.property('runTime');
      expect(q[0].runTime).to.be.greaterThan(0.0);
      expect(q[0].runTime).to.be.lessThan(60.0);
      expect(q[0]).to.have.property('peakMemoryUsage');
      expect(q[0].peakMemoryUsage).to.be.lessThan(100 * 1024);
      expect(q[0]).to.have.property('state', 'executing');
      expect(q[0]).to.have.property('stream', false);
    });

    it('should not track queries if turned off', function () {
      testee.properties({
        enabled: false
      });
      sendQuery(1, false);
      expect(testee.current().filter(filterQueries).length).to.equal(0);
    });

    it('should work when tracking is turned off in the middle', function () {
      sendQuery(1, true);
      expect(testee.current().filter(filterQueries).length).to.equal(1);

      // turn it off now
      testee.properties({
        enabled: false
      });

      // wait a while, and expect the list of queries to be empty in the end
      for (let tries = 0; tries < 20; tries++) {
        if (testee.current().filter(filterQueries).length === 0) {
          break;
        }
        internal.wait(1, false);
      }

      expect(testee.current().filter(filterQueries).length).to.equal(0);
    });

    it('should track slow queries by threshold', function () {
      let now = (new Date()).toISOString();
      sendQuery(1, false);
      expect(testee.current().filter(filterQueries).length).to.equal(0);
      expect(testee.slow().filter(filterQueries).length).to.equal(0);

      testee.properties({
        slowQueryThreshold: 2
      });

      sendQuery(1, false);
      expect(testee.current().filter(filterQueries).length).to.equal(0);
      let queries = testee.slow().filter(filterQueries);
      expect(queries.length).to.equal(1);
      expect(queries[0]).to.have.property('id');
      expect(queries[0]).to.have.property('database', '_system');
      expect(queries[0]).to.have.property('user', 'root');
      expect(queries[0]).to.have.property('query', query);
      expect(queries[0]).to.have.property('bindVars');
      expect(queries[0].bindVars).to.eql({ value: 1 });
      expect(queries[0]).to.have.property('started');
      expect(queries[0].started).to.be.greaterThan(now);
      expect(queries[0]).to.have.property('runTime');
      expect(queries[0].runTime).to.be.greaterThan(0.0);
      expect(queries[0].runTime).to.be.lessThan(60.0);
      expect(queries[0]).to.have.property('peakMemoryUsage');
      expect(queries[0].peakMemoryUsage).to.be.lessThan(100 * 1024);
      expect(queries[0]).to.have.property('state', 'finished');
      expect(queries[0]).to.have.property('stream', false);
    });
    
    it('should track peak memory usage', function () {
      const query = 'RETURN (FOR x IN 1..@amount FILTER SLEEP(0.0001) RETURN CONCAT("testmann", x))';
      const filterQueries = (q) => q.query === query;
      
      testee.properties({
        slowQueryThreshold: 2
      });

      let now = (new Date()).toISOString();
      internal.db._query(query, { amount: 20000 });
      expect(testee.current().filter(filterQueries).length).to.equal(0);
      let queries = testee.slow().filter(filterQueries);
      expect(queries.length).to.equal(1);
      expect(queries[0]).to.have.property('id');
      expect(queries[0]).to.have.property('database', '_system');
      expect(queries[0]).to.have.property('user', 'root');
      expect(queries[0]).to.have.property('query', query);
      expect(queries[0]).to.have.property('bindVars');
      expect(queries[0].bindVars).to.eql({ amount: 20000 });
      expect(queries[0]).to.have.property('started');
      expect(queries[0].started).to.be.greaterThan(now);
      expect(queries[0]).to.have.property('runTime');
      expect(queries[0].runTime).to.be.greaterThan(0.0);
      expect(queries[0].runTime).to.be.lessThan(60.0);
      expect(queries[0]).to.have.property('peakMemoryUsage');
      expect(queries[0].peakMemoryUsage).to.be.lessThan(100 * 1024);
      expect(queries[0]).to.have.property('state', 'finished');
      expect(queries[0]).to.have.property('stream', false);
    });

    it('should be able to clear the list of slow queries', function () {
      testee.properties({
        slowQueryThreshold: 2
      });
      sendQuery(1, false);
      expect(testee.slow().filter(filterQueries).length).to.equal(1);
      testee.clearSlow();
      expect(testee.slow().filter(filterQueries).length).to.equal(0);
    });

    it('should track at most n slow queries', function () {
      const max = 2;
      testee.properties({
        slowQueryThreshold: 2,
        maxSlowQueries: max
      });
      sendQuery(3, false);
      expect(testee.current().filter(filterQueries).length).to.equal(0);
      expect(testee.slow().filter(filterQueries).length).to.equal(max);
    });

    if (isServer && internal.debugCanUseFailAt()) {
      it('should not crash when trying to move a query into the slow list', function () {
        internal.debugSetFailAt('QueryList::remove');

        testee.properties({
          slowQueryThreshold: 2
        });
        sendQuery(3, false);

        // no slow queries should have been logged
        expect(testee.slow().filter(filterQueries).length).to.equal(0);
      });
    }

    it('should not track slow queries if turned off', function () {
      testee.properties({
        slowQueryThreshold: 2,
        trackSlowQueries: false
      });
      sendQuery(1, false);
      expect(testee.current().filter(filterQueries).length).to.equal(0);
      expect(testee.slow().filter(filterQueries).length).to.equal(0);
    });
    
    it('should not track slow queries if turned off but bind vars tracking is on', function () {
      testee.properties({
        slowQueryThreshold: 2,
        trackSlowQueries: false,
        trackBindVars: false
      });
      sendQuery(1, false);
      expect(testee.current().filter(filterQueries).length).to.equal(0);
      expect(testee.slow().filter(filterQueries).length).to.equal(0);
    });
    
    it('should track slow queries if only bind vars tracking is turned off', function () {
      testee.properties({
        slowQueryThreshold: 2,
        trackSlowQueries: true,
        trackBindVars: false
      });
      sendQuery(1, true);
      let q;
      let counter = 0;
      while (++counter < 10) {
        q = testee.current().filter(filterQueries);
        if (q.length > 0) {
          break;
        }
        internal.wait(0.25, false);
      }
      expect(q.length).to.equal(1);
    });

    it('should be able to kill a query', function () {
      sendQuery(1, true);
      let q;
      let counter = 0;
      while (++counter < 10) {
        q = testee.current().filter(filterQueries);
        if (q.length > 0) {
          break;
        }
        internal.wait(0.25, false);
      }
      expect(q.length).to.equal(1);
      expect(function () {
        testee.kill(q[0].id);
        let list = testee.current().filter(filterQueries);
        for (let i = 0; i < 10 && list.length > 0; ++i) {
          internal.wait(0.1);
          list = testee.current().filter(filterQueries);
        }
        expect(list.length).to.equal(0);
      }).not.to.throw();
    });
  });
});
