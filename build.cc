#include "build.h"

#include <stdio.h>

#include "ninja.h"

bool Plan::AddTarget(Node* node, string* err) {
  Edge* edge = node->in_edge_;
  if (!edge) {  // Leaf node.
    if (node->dirty_) {
      *err = "'" + node->file_->path_ + "' missing "
             "and no known rule to make it";
    }
    return false;
  }

  assert(edge);
  if (!node->dirty())
    return false;  // Don't need to do anything.
  if (want_.find(edge) != want_.end())
    return true;  // We've already enqueued it.

  bool awaiting_inputs = false;
  for (vector<Node*>::iterator i = edge->inputs_.begin();
       i != edge->inputs_.end(); ++i) {
    if (AddTarget(*i, err))
      awaiting_inputs = true;
    else if (err && !err->empty())
      return false;
  }

  want_.insert(edge);
  if (!awaiting_inputs)
    ready_.insert(edge);

  return true;
}

Edge* Plan::FindWork() {
  if (ready_.empty())
    return NULL;
  set<Edge*>::iterator i = ready_.begin();
  Edge* edge = *i;
  ready_.erase(i);
  return edge;
}

void Plan::EdgeFinished(Edge* edge) {
  want_.erase(edge);

  // Check off any nodes we were waiting for with this edge.
  for (vector<Node*>::iterator i = edge->outputs_.begin();
       i != edge->outputs_.end(); ++i) {
    NodeFinished(*i);
  }
}

void Plan::NodeFinished(Node* node) {
  // See if we we want any edges from this node.
  for (vector<Edge*>::iterator i = node->out_edges_.begin();
       i != node->out_edges_.end(); ++i) {
    if (want_.find(*i) != want_.end()) {
      // See if the edge is now ready.
      bool ready = true;
      for (vector<Node*>::iterator j = (*i)->inputs_.begin();
           j != (*i)->inputs_.end(); ++j) {
        if ((*j)->dirty()) {
          ready = false;
          break;
        }
      }
      if (ready)
        ready_.insert(*i);
    }
  }
}

void Plan::Dump() {
  printf("pending: %d\n", (int)want_.size());
  for (set<Edge*>::iterator i = want_.begin(); i != want_.end(); ++i) {
    (*i)->Dump();
  }
  printf("ready: %d\n", (int)ready_.size());
}

bool Shell::RunCommand(Edge* edge) {
  string err;
  string command = edge->EvaluateCommand();
  printf("  %s\n", command.c_str());
  int ret = system(command.c_str());
  if (WIFEXITED(ret)) {
    int exit = WEXITSTATUS(ret);
    if (exit == 0)
      return true;
    err = "nonzero exit status";
  } else {
    err = "something else went wrong";
  }
  return false;
}

Builder::Builder(State* state)
    : state_(state) {
  disk_interface_ = new RealDiskInterface;
}

Node* Builder::AddTarget(const string& name, string* err) {
  Node* node = state_->LookupNode(name);
  if (!node) {
    *err = "unknown target: '" + name + "'";
    return NULL;
  }
  node->file_->StatIfNecessary(disk_interface_);
  if (node->in_edge_) {
    if (!node->in_edge_->RecomputeDirty(state_, disk_interface_, err))
      return NULL;
  }
  if (!node->dirty_)
    return NULL;  // Intentionally no error.

  if (!plan_.AddTarget(node, err))
    return NULL;
  return node;
}

bool Builder::Build(Shell* shell, string* err) {
  if (!plan_.more_to_do()) {
    *err = "no work to do";
    return true;
  }

  Edge* edge = plan_.FindWork();
  if (!edge) {
    *err = "unable to find work";
    return false;
  }

  do {
    // Create directories necessary for outputs.
    for (vector<Node*>::iterator i = edge->outputs_.begin();
         i != edge->outputs_.end(); ++i) {
      if (!disk_interface_->MakeDirs((*i)->file_->path_))
        return false;
    }

    if (edge->rule_ != &State::kPhonyRule) {
      string command = edge->EvaluateCommand();
      if (!shell->RunCommand(edge)) {
        err->assign("command '" + command + "' failed.");
        return false;
      }
      for (vector<Node*>::iterator i = edge->outputs_.begin();
           i != edge->outputs_.end(); ++i) {
        // XXX check that the output actually changed
        // XXX just notify node and have it propagate?
        (*i)->dirty_ = false;
      }
    }
    plan_.EdgeFinished(edge);
  } while ((edge = plan_.FindWork()) != NULL);

  if (plan_.more_to_do()) {
    *err = "ran out of work";
    plan_.Dump();
    return false;
  }

  return true;
}
